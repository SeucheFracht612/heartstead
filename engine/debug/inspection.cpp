#include "engine/debug/inspection.hpp"
#include "engine/core/ids.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace heartstead::debug {

namespace {

[[nodiscard]] std::string bool_text(bool value) {
    return value ? "true" : "false";
}

[[nodiscard]] std::string save_id_text(core::SaveId id) {
    return id.is_valid() ? id.to_string() : "";
}

[[nodiscard]] std::string net_id_text(core::NetId id) {
    return id.is_valid() ? id.to_string() : "";
}

[[nodiscard]] std::string runtime_handle_text(core::RuntimeHandle id) {
    return id.is_valid() ? id.to_string() : "";
}

[[nodiscard]] std::string process_id_text(core::ProcessId id) {
    return id.is_valid() ? id.to_string() : "";
}

[[nodiscard]] std::string virtual_path_text(const assets::VirtualPath& path) {
    return path.to_string();
}

[[nodiscard]] bool is_valid_relative_path(const std::filesystem::path& path) {
    return core::is_valid_local_id(path.generic_string());
}

[[nodiscard]] std::string prototype_text(const core::PrototypeId& id) {
    return id.is_valid() ? id.value() : "";
}

void add_field(InspectionData& data, std::string name, std::string value) {
    data.fields.push_back(InspectionField{std::move(name), std::move(value)});
}

void add_issue(InspectionData& data, InspectionSeverity severity, std::string code,
               std::string message) {
    data.issues.push_back(InspectionIssue{severity, std::move(code), std::move(message)});
}

void add_status_issue(InspectionData& data, const core::Status& status) {
    if (!status) {
        add_issue(data, InspectionSeverity::error, status.error().code, status.error().message);
    }
}

void accumulate_saturated(std::uint64_t& total, std::uint64_t value, bool& saturated) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (total > maximum - value) {
        total = maximum;
        saturated = true;
        return;
    }
    total += value;
}

[[nodiscard]] simulation::WorldTick
simulation_catch_up_limit(simulation::SimulationLod lod,
                          const simulation::SimulationTickBudget& budget) noexcept {
    switch (lod) {
    case simulation::SimulationLod::full:
        return budget.maximum_full_catch_up_delta_ms_per_update;
    case simulation::SimulationLod::simplified:
        return budget.maximum_simplified_catch_up_delta_ms_per_update;
    case simulation::SimulationLod::sleeping:
        return budget.maximum_sleeping_catch_up_delta_ms_per_update;
    case simulation::SimulationLod::unloaded:
        return 0;
    }
    return 0;
}

[[nodiscard]] constexpr std::array<dirty::DirtyRegionKind, 12> all_dirty_region_kinds() noexcept {
    return {
        dirty::DirtyRegionKind::chunk_mesh,
        dirty::DirtyRegionKind::chunk_collision,
        dirty::DirtyRegionKind::chunk_lighting,
        dirty::DirtyRegionKind::room_graph,
        dirty::DirtyRegionKind::road_network,
        dirty::DirtyRegionKind::cart_access_network,
        dirty::DirtyRegionKind::storage_access_network,
        dirty::DirtyRegionKind::power_network,
        dirty::DirtyRegionKind::ward_network,
        dirty::DirtyRegionKind::smoke_ventilation_network,
        dirty::DirtyRegionKind::water_network,
        dirty::DirtyRegionKind::logistics_network,
    };
}

[[nodiscard]] std::string dirty_bounds_text(const dirty::DirtyRegionBounds& bounds) {
    std::ostringstream output;
    output << bounds.min.x << '|' << bounds.min.y << '|' << bounds.min.z << ".." << bounds.max.x
           << '|' << bounds.max.y << '|' << bounds.max.z;
    return output.str();
}

void add_dirty_region_count_fields(InspectionData& data, const dirty::DirtyRegionTracker& tracker) {
    for (const auto kind : all_dirty_region_kinds()) {
        add_field(data,
                  "dirty_region_" + std::string(dirty::dirty_region_kind_name(kind)) + "_count",
                  std::to_string(tracker.count(kind)));
    }
}

[[nodiscard]] std::string construction_state_text(build::ConstructionState state) {
    switch (state) {
    case build::ConstructionState::planned:
        return "planned";
    case build::ConstructionState::under_construction:
        return "under_construction";
    case build::ConstructionState::complete:
        return "complete";
    case build::ConstructionState::damaged:
        return "damaged";
    }
    return "unknown";
}

[[nodiscard]] std::string entity_kind_text(entities::EntityKind kind) {
    switch (kind) {
    case entities::EntityKind::player:
        return "player";
    case entities::EntityKind::creature:
        return "creature";
    case entities::EntityKind::animal:
        return "animal";
    case entities::EntityKind::cart:
        return "cart";
    case entities::EntityKind::boat:
        return "boat";
    case entities::EntityKind::dropped_item:
        return "dropped_item";
    case entities::EntityKind::projectile:
        return "projectile";
    case entities::EntityKind::machine:
        return "machine";
    case entities::EntityKind::container:
        return "container";
    case entities::EntityKind::temporary_physics:
        return "temporary_physics";
    }
    return "unknown";
}

[[nodiscard]] std::string process_state_text(processes::ProcessState state) {
    switch (state) {
    case processes::ProcessState::running:
        return "running";
    case processes::ProcessState::interrupted:
        return "interrupted";
    case processes::ProcessState::complete:
        return "complete";
    }
    return "unknown";
}

[[nodiscard]] std::string
descriptor_severity_counts(const std::vector<rooms::RoomDescriptor>& descriptors) {
    std::size_t positive = 0;
    std::size_t neutral = 0;
    std::size_t warning = 0;
    for (const auto& descriptor : descriptors) {
        switch (descriptor.severity) {
        case rooms::RoomDescriptorSeverity::positive:
            ++positive;
            break;
        case rooms::RoomDescriptorSeverity::neutral:
            ++neutral;
            break;
        case rooms::RoomDescriptorSeverity::warning:
            ++warning;
            break;
        }
    }

    std::ostringstream output;
    output << "positive=" << positive << ",neutral=" << neutral << ",warning=" << warning;
    return output.str();
}

[[nodiscard]] std::string workpiece_material_counts(const workpieces::WorkpieceGrid& grid) {
    std::map<std::uint16_t, std::size_t> counts;
    const auto shape = grid.shape();
    for (std::uint16_t z = 0; z < shape.depth; ++z) {
        for (std::uint16_t y = 0; y < shape.height; ++y) {
            for (std::uint16_t x = 0; x < shape.width; ++x) {
                auto cell = grid.get({x, y, z});
                if (cell && cell.value().is_occupied()) {
                    ++counts[cell.value().material];
                }
            }
        }
    }

    std::ostringstream output;
    bool first = true;
    for (const auto& [material, count] : counts) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << material << '=' << count;
    }
    return output.str();
}

[[nodiscard]] std::string
assembly_port_sources(const std::vector<assemblies::AssemblyPort>& ports) {
    std::ostringstream output;
    bool first = true;
    for (const auto& port : ports) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << port.name << ':' << save_id_text(port.source_build_piece_id) << ':'
               << port.capacity;
    }
    return output.str();
}

[[nodiscard]] std::uint64_t
assembly_total_port_capacity(const std::vector<assemblies::AssemblyPort>& ports) noexcept {
    std::uint64_t total = 0;
    for (const auto& port : ports) {
        total += port.capacity;
    }
    return total;
}

[[nodiscard]] std::string severity_text(InspectionSeverity severity) {
    switch (severity) {
    case InspectionSeverity::info:
        return "info";
    case InspectionSeverity::warning:
        return "warning";
    case InspectionSeverity::error:
        return "error";
    }
    return "unknown";
}

[[nodiscard]] InspectionSeverity inspection_severity(modding::DiagnosticSeverity severity) {
    switch (severity) {
    case modding::DiagnosticSeverity::info:
        return InspectionSeverity::info;
    case modding::DiagnosticSeverity::warning:
        return InspectionSeverity::warning;
    case modding::DiagnosticSeverity::error:
        return InspectionSeverity::error;
    }
    return InspectionSeverity::warning;
}

[[nodiscard]] std::string comma_join(const std::vector<std::string>& values) {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            output << ',';
        }
        output << values[index];
    }
    return output.str();
}

[[nodiscard]] bool has_world_content(const world::WorldStateStats& stats) noexcept {
    return stats.chunk_count > 0 || stats.region_count > 0 || stats.region_connection_count > 0 ||
           stats.dirty_region_count > 0 || stats.build_object_count > 0 || stats.entity_count > 0 ||
           stats.cargo_count > 0 || stats.inventory_count > 0 || stats.workpiece_count > 0 ||
           stats.physical_resource_count > 0 || stats.assembly_count > 0 ||
           stats.process_count > 0 || stats.room_count > 0 || stats.network_count > 0 ||
           stats.mod_state_count > 0 || stats.missing_prototype_count > 0 || stats.fire_count > 0;
}

void add_simulation_frame_plan_issues(InspectionData& data,
                                      const simulation::SimulationFramePlan& plan) {
    std::size_t full_count = 0;
    std::size_t simplified_count = 0;
    std::size_t sleeping_count = 0;
    std::size_t unloaded_count = 0;
    std::size_t due_tick_count = 0;

    for (const auto& decision : plan.decisions) {
        switch (decision.lod) {
        case simulation::SimulationLod::full:
            ++full_count;
            break;
        case simulation::SimulationLod::simplified:
            ++simplified_count;
            break;
        case simulation::SimulationLod::sleeping:
            ++sleeping_count;
            break;
        case simulation::SimulationLod::unloaded:
            ++unloaded_count;
            break;
        }
        if (decision.due_for_tick) {
            ++due_tick_count;
        }
    }

    if (full_count != plan.full_count || simplified_count != plan.simplified_count ||
        sleeping_count != plan.sleeping_count || unloaded_count != plan.unloaded_count ||
        due_tick_count != plan.due_tick_count) {
        add_issue(data, InspectionSeverity::error, "simulation.frame_plan_count_mismatch",
                  "simulation frame plan counters do not match decision contents");
    }
}

void add_budgeted_simulation_frame_plan_issues(
    InspectionData& data, const simulation::BudgetedSimulationFramePlan& plan) {
    add_simulation_frame_plan_issues(data, plan.frame);
    add_status_issue(data, plan.budget.validate());
    if (plan.updates.size() > plan.budget.maximum_updates_per_tick ||
        plan.scheduled_work_units > plan.budget.maximum_work_units_per_tick ||
        plan.scheduled_catch_up_delta_ms > plan.budget.maximum_catch_up_delta_ms_per_tick) {
        add_issue(data, InspectionSeverity::error, "simulation.budget_plan_limit_exceeded",
                  "scheduled simulation work exceeds its recorded hard budget");
    }

    std::vector<const simulation::ScheduledSimulationUpdate*> scheduled(plan.frame.decisions.size(),
                                                                        nullptr);
    std::uint64_t scheduled_work_units = 0;
    simulation::WorldTick scheduled_delta_ms = 0;
    simulation::WorldTick scheduled_catch_up_delta_ms = 0;
    bool calculated_saturated = false;
    for (const auto& update : plan.updates) {
        if (update.decision_index >= plan.frame.decisions.size()) {
            add_issue(data, InspectionSeverity::error, "simulation.budget_plan_invalid_index",
                      "scheduled simulation update references a missing decision");
            continue;
        }
        if (scheduled[update.decision_index] != nullptr) {
            add_issue(data, InspectionSeverity::error, "simulation.budget_plan_duplicate_update",
                      "one simulation decision was scheduled more than once");
            continue;
        }
        scheduled[update.decision_index] = &update;
        const auto& decision = plan.frame.decisions[update.decision_index];
        if (!decision.due_for_tick || decision.tick_interval_ms == 0 ||
            decision.elapsed_since_update_ms > plan.now_ms ||
            update.update_delta_ms < decision.tick_interval_ms ||
            update.update_delta_ms > decision.elapsed_since_update_ms ||
            update.remaining_delta_ms !=
                decision.elapsed_since_update_ms - update.update_delta_ms ||
            update.catch_up_delta_ms != update.update_delta_ms - decision.tick_interval_ms ||
            update.catch_up_delta_ms > simulation_catch_up_limit(decision.lod, plan.budget) ||
            update.next_update_time_ms !=
                plan.now_ms - decision.elapsed_since_update_ms + update.update_delta_ms ||
            update.estimated_work_units != decision.estimated_update_work_units) {
            add_issue(data, InspectionSeverity::error, "simulation.budget_plan_invalid_update",
                      "scheduled simulation update does not match its due decision");
        }
        accumulate_saturated(scheduled_work_units, update.estimated_work_units,
                             calculated_saturated);
        accumulate_saturated(scheduled_delta_ms, update.update_delta_ms, calculated_saturated);
        accumulate_saturated(scheduled_catch_up_delta_ms, update.catch_up_delta_ms,
                             calculated_saturated);
    }

    std::size_t deferred_due_count = 0;
    std::size_t catch_up_due_count = 0;
    std::size_t remaining_catch_up_count = 0;
    std::uint64_t due_work_units = 0;
    std::uint64_t deferred_work_units = 0;
    simulation::WorldTick deferred_delta_ms = 0;
    simulation::WorldTick deferred_catch_up_delta_ms = 0;
    simulation::WorldTick maximum_lateness_ms = 0;
    for (std::size_t index = 0; index < plan.frame.decisions.size(); ++index) {
        const auto& decision = plan.frame.decisions[index];
        if (!decision.due_for_tick) {
            continue;
        }
        if (decision.tick_interval_ms == 0 ||
            decision.elapsed_since_update_ms < decision.tick_interval_ms) {
            add_issue(data, InspectionSeverity::error, "simulation.budget_plan_invalid_due",
                      "budgeted simulation plan contains an invalid due decision");
            continue;
        }

        accumulate_saturated(due_work_units, decision.estimated_update_work_units,
                             calculated_saturated);
        const auto lateness = decision.elapsed_since_update_ms - decision.tick_interval_ms;
        maximum_lateness_ms = std::max(maximum_lateness_ms, lateness);
        if (lateness > 0) {
            ++catch_up_due_count;
        }

        if (scheduled[index] == nullptr) {
            ++deferred_due_count;
            accumulate_saturated(deferred_work_units, decision.estimated_update_work_units,
                                 calculated_saturated);
            accumulate_saturated(deferred_delta_ms, decision.elapsed_since_update_ms,
                                 calculated_saturated);
            accumulate_saturated(deferred_catch_up_delta_ms, lateness, calculated_saturated);
            if (lateness > 0) {
                ++remaining_catch_up_count;
            }
            continue;
        }

        const auto remaining = scheduled[index]->remaining_delta_ms;
        if (remaining >= decision.tick_interval_ms) {
            ++remaining_catch_up_count;
            accumulate_saturated(deferred_delta_ms, remaining, calculated_saturated);
            accumulate_saturated(deferred_catch_up_delta_ms, remaining - decision.tick_interval_ms,
                                 calculated_saturated);
        }
    }

    const auto expected_budget_exhausted = deferred_due_count > 0 || remaining_catch_up_count > 0;
    const auto telemetry_mismatch =
        scheduled_work_units != plan.scheduled_work_units ||
        scheduled_delta_ms != plan.scheduled_delta_ms ||
        scheduled_catch_up_delta_ms != plan.scheduled_catch_up_delta_ms ||
        deferred_due_count != plan.deferred_due_count ||
        catch_up_due_count != plan.catch_up_due_count ||
        remaining_catch_up_count != plan.remaining_catch_up_count ||
        due_work_units != plan.due_work_units || deferred_work_units != plan.deferred_work_units ||
        deferred_delta_ms != plan.deferred_delta_ms ||
        deferred_catch_up_delta_ms != plan.deferred_catch_up_delta_ms ||
        maximum_lateness_ms != plan.maximum_lateness_ms ||
        expected_budget_exhausted != plan.budget_exhausted ||
        calculated_saturated != plan.counters_saturated;
    if (telemetry_mismatch) {
        add_issue(data, InspectionSeverity::error, "simulation.budget_plan_telemetry_mismatch",
                  "budgeted simulation telemetry does not match scheduled updates");
    }
    if (plan.counters_saturated) {
        add_issue(data, InspectionSeverity::warning, "simulation.budget_plan_counter_saturated",
                  "budgeted simulation time or work telemetry saturated");
    }
    if (plan.budget_exhausted) {
        add_issue(data, InspectionSeverity::info, "simulation.budget_plan_backlog",
                  "simulation work or catch-up debt remains after this tick");
    }
}

[[nodiscard]] std::string
operation_stage_sequence(const std::vector<world::OperationStage>& stages) {
    std::ostringstream output;
    bool first = true;
    for (const auto stage : stages) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << world::operation_stage_name(stage);
    }
    return output.str();
}

[[nodiscard]] std::string operation_trace_state(const net::CommandOperationTrace& trace) {
    if (trace.stages.empty()) {
        return "empty";
    }

    const auto last_stage = trace.stages.back();
    if (last_stage == world::OperationStage::committed) {
        return "committed";
    }
    if (last_stage == world::OperationStage::rolled_back) {
        return "rolled_back";
    }
    if (trace.mutations.empty()) {
        return "query";
    }
    return "open";
}

void add_operation_trace_fields(InspectionData& data, const net::CommandOperationTrace& trace) {
    add_field(data, "stage_count", std::to_string(trace.stages.size()));
    add_field(data, "stage_sequence", operation_stage_sequence(trace.stages));
    add_field(data, "last_stage",
              trace.stages.empty() ? ""
                                   : std::string(world::operation_stage_name(trace.stages.back())));
    add_field(data, "mutation_count", std::to_string(trace.mutations.size()));
    add_field(data, "derived_update_count", std::to_string(trace.derived_updates.size()));
    add_field(data, "replication_dirty", bool_text(trace.replication_dirty));
    add_field(data, "save_dirty", bool_text(trace.save_dirty));
    add_field(data, "first_mutation", trace.mutations.empty() ? "" : trace.mutations.front());
    add_field(data, "first_derived_update",
              trace.derived_updates.empty() ? "" : trace.derived_updates.front());
}

void add_operation_trace_issues(InspectionData& data, const net::CommandOperationTrace& trace) {
    if (trace.stages.empty()) {
        add_issue(data, InspectionSeverity::warning, "command_trace.empty",
                  "command operation trace has no stages");
        return;
    }

    const auto committed = trace.stages.back() == world::OperationStage::committed;
    if (committed && trace.mutations.empty()) {
        add_issue(data, InspectionSeverity::error, "command_trace.committed_without_mutation",
                  "committed command operation trace has no mutation");
    }
    if (!trace.mutations.empty() && !trace.replication_dirty) {
        add_issue(data, committed ? InspectionSeverity::error : InspectionSeverity::warning,
                  "command_trace.replication_not_dirty",
                  "command operation trace has mutations but replication is not dirty");
    }
    if (!trace.mutations.empty() && !trace.save_dirty) {
        add_issue(data, committed ? InspectionSeverity::error : InspectionSeverity::warning,
                  "command_trace.save_not_dirty",
                  "command operation trace has mutations but save data is not dirty");
    }
}

[[nodiscard]] std::uint32_t relevance_relevant_client_total(
    const std::vector<net::ReplicationRelevanceReport>& reports) noexcept {
    std::uint32_t total = 0;
    for (const auto& report : reports) {
        total += report.relevant_client_count;
    }
    return total;
}

[[nodiscard]] std::uint32_t relevance_filtered_client_total(
    const std::vector<net::ReplicationRelevanceReport>& reports) noexcept {
    std::uint32_t total = 0;
    for (const auto& report : reports) {
        total += report.filtered_client_count;
    }
    return total;
}

[[nodiscard]] std::uint32_t world_interest_visible_subject_total(
    const std::vector<world::WorldReplicationViewerInterestReport>& reports) noexcept {
    std::uint32_t total = 0;
    for (const auto& report : reports) {
        total += report.visible_subject_count;
    }
    return total;
}

[[nodiscard]] std::uint32_t world_interest_excluded_lod_subject_total(
    const std::vector<world::WorldReplicationViewerInterestReport>& reports) noexcept {
    std::uint32_t total = 0;
    for (const auto& report : reports) {
        total += report.excluded_lod_subject_count;
    }
    return total;
}

[[nodiscard]] std::uint32_t world_interest_skipped_non_saved_subject_total(
    const std::vector<world::WorldReplicationViewerInterestReport>& reports) noexcept {
    std::uint32_t total = 0;
    for (const auto& report : reports) {
        total += report.skipped_non_saved_subject_count;
    }
    return total;
}

[[nodiscard]] std::string
script_permission_list(const std::vector<scripting::ScriptPermission>& permissions) {
    std::ostringstream output;
    bool first = true;
    for (const auto permission : permissions) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << scripting::script_permission_name(permission);
    }
    return output.str();
}

[[nodiscard]] std::string
script_host_api_argument_list(const std::vector<scripting::ScriptHostApiArgument>& arguments) {
    std::ostringstream output;
    bool first = true;
    for (const auto& argument : arguments) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << argument.name << ':' << scripting::script_value_kind_name(argument.kind);
        if (argument.optional) {
            output << '?';
        }
    }
    return output.str();
}

[[nodiscard]] std::uint32_t source_limit_for_inspection(std::size_t source_bytes) noexcept {
    if (source_bytes == 0) {
        return 1;
    }
    if (source_bytes > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(source_bytes);
}

} // namespace

bool InspectionData::has_errors() const noexcept {
    return std::ranges::any_of(issues, [](const InspectionIssue& issue) {
        return issue.severity == InspectionSeverity::error;
    });
}

const InspectionField* InspectionData::find_field(std::string_view name) const noexcept {
    const auto found = std::ranges::find_if(
        fields, [name](const InspectionField& field) { return field.name == name; });
    return found == fields.end() ? nullptr : &*found;
}

InspectionData Inspector::inspect(const platform::PlatformBackendCapabilities& capabilities) {
    InspectionData data;
    data.object_type = "platform_backend_capabilities";
    data.display_name =
        "Platform Backend " + std::string(platform::platform_backend_name(capabilities.backend));
    data.state = capabilities.available ? "available" : "unavailable";
    add_field(data, "backend", std::string(platform::platform_backend_name(capabilities.backend)));
    add_field(data, "available", bool_text(capabilities.available));
    add_field(data, "headless", bool_text(capabilities.headless));
    add_field(data, "supports_logical_windows", bool_text(capabilities.supports_logical_windows));
    add_field(data, "supports_native_windows", bool_text(capabilities.supports_native_windows));
    add_field(data, "supports_keyboard_input", bool_text(capabilities.supports_keyboard_input));
    add_field(data, "supports_text_input", bool_text(capabilities.supports_text_input));
    add_field(data, "supports_mouse_input", bool_text(capabilities.supports_mouse_input));
    add_field(data, "supports_display_metadata", bool_text(capabilities.supports_display_metadata));
    add_field(data, "supports_vulkan_surface", bool_text(capabilities.supports_vulkan_surface));
    add_field(data, "supports_clipboard", bool_text(capabilities.supports_clipboard));
    add_field(data, "window_system", std::string(capabilities.window_system));
    if (!capabilities.available) {
        add_issue(data, InspectionSeverity::warning, "platform.backend_unavailable",
                  "platform backend is not available");
    }
    return data;
}

InspectionData Inspector::inspect(const platform::DisplayInfo& display) {
    InspectionData data;
    data.object_type = "platform_display";
    data.display_name =
        display.name.empty() ? "Display " + std::to_string(display.index) : display.name;
    data.state = display.primary ? "primary" : "available";
    add_field(data, "index", std::to_string(display.index));
    add_field(data, "name", display.name);
    add_field(data, "x_px", std::to_string(display.x_px));
    add_field(data, "y_px", std::to_string(display.y_px));
    add_field(data, "width_px", std::to_string(display.width_px));
    add_field(data, "height_px", std::to_string(display.height_px));
    add_field(data, "width_mm", std::to_string(display.width_mm));
    add_field(data, "height_mm", std::to_string(display.height_mm));
    add_field(data, "dpi_x", std::to_string(display.dpi_x));
    add_field(data, "dpi_y", std::to_string(display.dpi_y));
    add_field(data, "refresh_hz", std::to_string(display.refresh_hz));
    add_field(data, "primary", bool_text(display.primary));
    if (display.width_px == 0 || display.height_px == 0) {
        add_issue(data, InspectionSeverity::warning, "platform.display_empty_extent",
                  "display pixel dimensions are not available");
    }
    return data;
}

InspectionData Inspector::inspect(const renderer::rhi::RenderBackendCapabilities& capabilities) {
    InspectionData data;
    data.object_type = "render_backend_capabilities";
    data.display_name =
        "Render Backend " + std::string(renderer::rhi::render_backend_name(capabilities.backend));
    data.state = capabilities.available ? "available" : "unavailable";
    add_field(data, "backend",
              std::string(renderer::rhi::render_backend_name(capabilities.backend)));
    add_field(data, "available", bool_text(capabilities.available));
    add_field(data, "supports_present", bool_text(capabilities.supports_present));
    add_field(data, "supports_validation", bool_text(capabilities.supports_validation));
    add_field(data, "supports_debug_markers", bool_text(capabilities.supports_debug_markers));
    add_field(data, "supports_shader_modules", bool_text(capabilities.supports_shader_modules));
    add_field(data, "supports_pipeline_layout", bool_text(capabilities.supports_pipeline_layout));
    add_field(data, "supports_compute_pipelines",
              bool_text(capabilities.supports_compute_pipelines));
    add_field(data, "supports_graphics_pipelines",
              bool_text(capabilities.supports_graphics_pipelines));
    add_field(data, "supports_descriptor_writes",
              bool_text(capabilities.supports_descriptor_writes));
    add_field(data, "supports_buffer_upload", bool_text(capabilities.supports_buffer_upload));
    add_field(data, "supports_image_upload", bool_text(capabilities.supports_image_upload));
    add_field(data, "supports_draw_binding", bool_text(capabilities.supports_draw_binding));
    add_field(data, "requires_window_surface", bool_text(capabilities.requires_window_surface));
    add_field(data, "requires_gpu_device", bool_text(capabilities.requires_gpu_device));
    add_field(data, "supports_headless", bool_text(capabilities.supports_headless));
    add_field(data, "recommended_frames_in_flight",
              std::to_string(capabilities.recommended_frames_in_flight));
    add_field(data, "graphics_api", std::string(capabilities.graphics_api));
    if (!capabilities.available) {
        add_issue(data, InspectionSeverity::warning, "renderer.backend_unavailable",
                  "renderer backend is not available");
    }
    return data;
}

InspectionData Inspector::inspect(const physics::PhysicsBackendCapabilities& capabilities) {
    InspectionData data;
    data.object_type = "physics_backend_capabilities";
    data.display_name =
        "Physics Backend " + std::string(physics::physics_backend_name(capabilities.backend));
    data.state = capabilities.available ? "available" : "unavailable";
    add_field(data, "backend", std::string(physics::physics_backend_name(capabilities.backend)));
    add_field(data, "available", bool_text(capabilities.available));
    add_field(data, "deterministic", bool_text(capabilities.deterministic));
    add_field(data, "supports_dynamic_bodies", bool_text(capabilities.supports_dynamic_bodies));
    add_field(data, "supports_kinematic_bodies", bool_text(capabilities.supports_kinematic_bodies));
    add_field(data, "supports_static_bodies", bool_text(capabilities.supports_static_bodies));
    add_field(data, "supports_compound_shapes", bool_text(capabilities.supports_compound_shapes));
    add_field(data, "supports_aabb_queries", bool_text(capabilities.supports_aabb_queries));
    add_field(data, "supports_contacts", bool_text(capabilities.supports_contacts));
    add_field(data, "supports_sleeping", bool_text(capabilities.supports_sleeping));
    add_field(data, "supports_character_controllers",
              bool_text(capabilities.supports_character_controllers));
    add_field(data, "supports_constraints", bool_text(capabilities.supports_constraints));
    add_field(data, "supports_collision_response",
              bool_text(capabilities.supports_collision_response));
    add_field(data, "library", std::string(capabilities.library));
    if (!capabilities.available) {
        add_issue(data, InspectionSeverity::warning, "physics.backend_unavailable",
                  "physics backend is not available");
    }
    return data;
}

InspectionData Inspector::inspect(const assets::MountPoint& mount) {
    InspectionData data;
    data.object_type = "vfs_mount";
    data.display_name = mount.namespace_id;
    data.runtime_id = mount.namespace_id;
    data.state = "mounted";
    add_field(data, "namespace_id", mount.namespace_id);
    add_field(data, "root", mount.root.generic_string());

    if (!core::is_valid_namespace_id(mount.namespace_id)) {
        add_issue(data, InspectionSeverity::error, "vfs.invalid_namespace",
                  "mount namespace is not valid");
    }
    std::error_code error;
    if (mount.root.empty()) {
        add_issue(data, InspectionSeverity::error, "vfs.empty_root", "mount root is empty");
    } else if (!std::filesystem::is_directory(mount.root, error) || error) {
        add_issue(data, InspectionSeverity::warning, "vfs.root_unavailable",
                  "mount root is not currently available");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const assets::VirtualFileEntry& entry) {
    InspectionData data;
    data.object_type = "vfs_file_entry";
    data.display_name = virtual_path_text(entry.virtual_path);
    data.runtime_id = virtual_path_text(entry.virtual_path);
    data.state = "resolved";
    add_field(data, "virtual_path", virtual_path_text(entry.virtual_path));
    add_field(data, "namespace_id", entry.virtual_path.namespace_id);
    add_field(data, "relative_path", entry.virtual_path.relative_path.generic_string());
    add_field(data, "resolved_path", entry.resolved_path.generic_string());
    add_field(data, "mount_index", std::to_string(entry.mount_index));

    if (!core::is_valid_namespace_id(entry.virtual_path.namespace_id)) {
        add_issue(data, InspectionSeverity::error, "vfs.invalid_namespace",
                  "entry namespace is not valid");
    }
    std::error_code error;
    if (entry.resolved_path.empty()) {
        add_issue(data, InspectionSeverity::error, "vfs.empty_resolved_path",
                  "resolved path is empty");
    } else if (!std::filesystem::is_regular_file(entry.resolved_path, error) || error) {
        add_issue(data, InspectionSeverity::warning, "vfs.file_unavailable",
                  "resolved file is not currently available");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const assets::VirtualFileSystem& vfs) {
    InspectionData data;
    data.object_type = "virtual_file_system";
    data.display_name = "Virtual File System";
    data.state = vfs.mounts().empty() ? "empty" : "mounted";

    std::vector<std::string> namespaces;
    for (std::size_t index = 0; index < vfs.mounts().size(); ++index) {
        const auto& mount = vfs.mounts()[index];
        if (std::ranges::find(namespaces, mount.namespace_id) == namespaces.end()) {
            namespaces.push_back(mount.namespace_id);
        }
        const auto prefix = "mount_" + std::to_string(index) + '_';
        add_field(data, prefix + "namespace_id", mount.namespace_id);
        add_field(data, prefix + "root", mount.root.generic_string());
    }
    std::ranges::sort(namespaces);

    add_field(data, "mount_count", std::to_string(vfs.mounts().size()));
    add_field(data, "namespace_count", std::to_string(namespaces.size()));
    add_field(data, "namespaces", comma_join(namespaces));

    if (vfs.mounts().empty()) {
        add_issue(data, InspectionSeverity::warning, "vfs.empty",
                  "virtual file system has no mounts");
    }
    for (const auto& mount : vfs.mounts()) {
        auto mount_inspection = inspect(mount);
        for (const auto& issue : mount_inspection.issues) {
            add_issue(data, issue.severity, issue.code, issue.message);
        }
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const assets::AssetRecord& record) {
    InspectionData data;
    data.object_type = "asset_record";
    data.display_name = record.logical_id;
    data.runtime_id = record.logical_id;
    data.state = record.cooked ? "cooked" : "source";
    add_field(data, "logical_id", record.logical_id);
    add_field(data, "kind", std::string(assets::asset_kind_name(record.kind)));
    add_field(data, "virtual_path", virtual_path_text(record.virtual_path));
    add_field(data, "virtual_namespace", record.virtual_path.namespace_id);
    add_field(data, "virtual_relative_path", record.virtual_path.relative_path.generic_string());
    add_field(data, "source_kind", std::string(assets::asset_source_kind_name(record.source_kind)));
    add_field(data, "source_id", record.source_id);
    add_field(data, "priority", std::to_string(record.priority));
    add_field(data, "source_path", record.source_path.generic_string());
    add_field(data, "content_hash", record.content_hash);
    add_field(data, "cooked", bool_text(record.cooked));
    add_field(data, "dependency_count", std::to_string(record.dependencies.size()));
    if (!record.dependencies.empty()) {
        add_field(data, "first_dependency", virtual_path_text(record.dependencies.front()));
    }

    if (!core::PrototypeId::parse(record.logical_id)) {
        add_issue(data, InspectionSeverity::error, "asset_record.invalid_logical_id",
                  "asset logical id is not a safe relative id");
    }
    if (!core::is_valid_namespace_id(record.virtual_path.namespace_id)) {
        add_issue(data, InspectionSeverity::error, "asset_record.invalid_namespace",
                  "asset virtual namespace is invalid");
    }
    if (record.logical_id != assets::asset_logical_id(record.virtual_path)) {
        add_issue(data, InspectionSeverity::warning, "asset_record.path_mismatch",
                  "asset logical id differs from its virtual relative path");
    }
    if (record.source_id.empty()) {
        add_issue(data, InspectionSeverity::error, "asset_record.missing_source",
                  "asset record needs a source id");
    }
    if (record.content_hash.empty()) {
        add_issue(data, InspectionSeverity::warning, "asset_record.missing_hash",
                  "asset record has no content hash");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const assets::AssetCatalog& catalog) {
    InspectionData data;
    data.object_type = "asset_catalog";
    data.display_name = "Asset Catalog";

    const auto records = catalog.records();
    const auto active_records = catalog.active_records();
    std::map<std::string, std::size_t> logical_counts;
    std::size_t invalid_record_count = 0;
    std::size_t warning_record_count = 0;
    std::size_t active_texture_count = 0;
    std::size_t active_model_count = 0;
    std::size_t active_shader_count = 0;

    for (const auto* record : records) {
        ++logical_counts[record->logical_id];
        auto record_inspection = inspect(*record);
        if (record_inspection.has_errors()) {
            ++invalid_record_count;
        }
        if (!record_inspection.issues.empty()) {
            ++warning_record_count;
        }
    }
    for (const auto* record : active_records) {
        switch (record->kind) {
        case assets::AssetKind::texture:
            ++active_texture_count;
            break;
        case assets::AssetKind::model:
            ++active_model_count;
            break;
        case assets::AssetKind::shader:
            ++active_shader_count;
            break;
        default:
            break;
        }
    }

    std::size_t override_logical_id_count = 0;
    for (const auto& [logical_id, count] : logical_counts) {
        (void)logical_id;
        if (count > 1) {
            ++override_logical_id_count;
        }
    }

    add_field(data, "record_count", std::to_string(catalog.record_count()));
    add_field(data, "active_count", std::to_string(catalog.active_count()));
    add_field(data, "inactive_count", std::to_string(records.size() - active_records.size()));
    add_field(data, "logical_id_count", std::to_string(logical_counts.size()));
    add_field(data, "override_logical_id_count", std::to_string(override_logical_id_count));
    add_field(data, "texture_count",
              std::to_string(catalog.count_kind(assets::AssetKind::texture)));
    add_field(data, "model_count", std::to_string(catalog.count_kind(assets::AssetKind::model)));
    add_field(data, "shader_count", std::to_string(catalog.count_kind(assets::AssetKind::shader)));
    add_field(data, "sound_count", std::to_string(catalog.count_kind(assets::AssetKind::sound)));
    add_field(data, "music_count", std::to_string(catalog.count_kind(assets::AssetKind::music)));
    add_field(data, "font_count", std::to_string(catalog.count_kind(assets::AssetKind::font)));
    add_field(data, "data_count", std::to_string(catalog.count_kind(assets::AssetKind::data)));
    add_field(data, "active_texture_count", std::to_string(active_texture_count));
    add_field(data, "active_model_count", std::to_string(active_model_count));
    add_field(data, "active_shader_count", std::to_string(active_shader_count));
    add_field(data, "invalid_record_count", std::to_string(invalid_record_count));
    add_field(data, "record_issue_count", std::to_string(warning_record_count));

    if (!records.empty()) {
        add_field(data, "first_record_logical_id", records.front()->logical_id);
        add_field(data, "first_record_source_id", records.front()->source_id);
    }
    if (!active_records.empty()) {
        add_field(data, "first_active_logical_id", active_records.front()->logical_id);
        add_field(data, "first_active_source_id", active_records.front()->source_id);
    }

    if (records.empty()) {
        add_issue(data, InspectionSeverity::warning, "asset_catalog.empty",
                  "asset catalog has no records");
    }
    if (catalog.active_count() > catalog.record_count()) {
        add_issue(data, InspectionSeverity::error, "asset_catalog.invalid_active_count",
                  "asset catalog has more active records than total records");
    }
    if (invalid_record_count > 0) {
        add_issue(data, InspectionSeverity::error, "asset_catalog.invalid_records",
                  "asset catalog contains invalid records");
    } else if (warning_record_count > 0) {
        add_issue(data, InspectionSeverity::warning, "asset_catalog.record_warnings",
                  "asset catalog contains records with inspection warnings");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    } else if (records.empty()) {
        data.state = "empty";
    } else if (override_logical_id_count > 0) {
        data.state = "overridden";
    } else {
        data.state = "indexed";
    }
    return data;
}

InspectionData Inspector::inspect(const assets::ResourcePackLoadPlan& plan) {
    InspectionData data;
    data.object_type = "resource_pack_load_plan";
    data.display_name = "Resource Pack Load Plan";
    data.state = plan.entries.empty() ? "empty" : "planned";
    add_field(data, "pack_count", std::to_string(plan.entries.size()));

    bool priority_order_valid = true;
    std::size_t invalid_pack_count = 0;
    for (std::size_t index = 0; index < plan.entries.size(); ++index) {
        const auto& entry = plan.entries[index];
        if (!core::is_valid_namespace_id(entry.manifest.id)) {
            ++invalid_pack_count;
        }
        if (entry.load_index != index) {
            priority_order_valid = false;
        }
        if (index > 0 && entry.asset_priority <= plan.entries[index - 1].asset_priority) {
            priority_order_valid = false;
        }
    }

    add_field(data, "invalid_pack_count", std::to_string(invalid_pack_count));
    add_field(data, "priority_order_valid", bool_text(priority_order_valid));
    if (!plan.entries.empty()) {
        const auto& first = plan.entries.front();
        const auto& last = plan.entries.back();
        add_field(data, "first_pack_id", first.manifest.id);
        add_field(data, "first_pack_priority", std::to_string(first.asset_priority));
        add_field(data, "last_pack_id", last.manifest.id);
        add_field(data, "last_pack_priority", std::to_string(last.asset_priority));
    }

    if (plan.entries.empty()) {
        add_issue(data, InspectionSeverity::warning, "resource_pack_load_plan.empty",
                  "resource pack load plan has no packs");
    }
    if (invalid_pack_count > 0) {
        add_issue(data, InspectionSeverity::error, "resource_pack_load_plan.invalid_pack",
                  "resource pack load plan contains invalid pack ids");
    }
    if (!priority_order_valid) {
        add_issue(data, InspectionSeverity::error, "resource_pack_load_plan.invalid_priority_order",
                  "resource pack load plan priorities must increase with load index");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const audio::SoundEventDefinition& event) {
    InspectionData data;
    data.object_type = "sound_event";
    data.display_name = "Sound Event";
    data.prototype_id = prototype_text(event.prototype_id);
    data.state = "valid";
    add_field(data, "asset_id", event.asset_id);
    add_field(data, "bus", std::string(audio::audio_bus_name(event.bus)));
    add_field(data, "gain", std::to_string(event.gain));
    add_field(data, "minimum_distance", std::to_string(event.minimum_distance));
    add_field(data, "maximum_distance", std::to_string(event.maximum_distance));
    add_field(data, "priority", std::to_string(event.priority));
    add_field(data, "maximum_instances", std::to_string(event.maximum_instances));
    add_field(data, "spatialized", bool_text(event.spatialized));
    add_field(data, "looping", bool_text(event.looping));
    add_field(data, "streaming", bool_text(event.streaming));
    auto status = event.validate();
    add_status_issue(data, status);
    if (!status) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const audio::SoundEventRegistry& registry) {
    InspectionData data;
    data.object_type = "sound_event_registry";
    data.display_name = "Sound Event Registry";
    data.state = "ready";
    std::size_t invalid_count = 0;
    std::size_t looping_count = 0;
    std::size_t streaming_count = 0;
    for (const auto& event : registry.definitions()) {
        if (!event.validate()) {
            ++invalid_count;
        }
        looping_count += event.looping ? 1U : 0U;
        streaming_count += event.streaming ? 1U : 0U;
    }
    add_field(data, "event_count", std::to_string(registry.size()));
    add_field(data, "looping_event_count", std::to_string(looping_count));
    add_field(data, "streaming_event_count", std::to_string(streaming_count));
    add_field(data, "invalid_event_count", std::to_string(invalid_count));
    if (invalid_count > 0) {
        data.state = "invalid";
        add_issue(data, InspectionSeverity::error, "sound_event_registry.invalid_events",
                  "sound event registry contains invalid definitions");
    }
    return data;
}

InspectionData Inspector::inspect(const audio::AudioSystemStats& stats) {
    InspectionData data;
    data.object_type = "audio_system_stats";
    data.display_name = "Audio System Stats";
    data.state = stats.active_voices == 0 ? "idle" : "active";
    add_field(data, "active_voices", std::to_string(stats.active_voices));
    add_field(data, "maximum_voices", std::to_string(stats.maximum_voices));
    add_field(data, "played_voices", std::to_string(stats.played_voices));
    add_field(data, "stopped_voices", std::to_string(stats.stopped_voices));
    add_field(data, "stolen_voices", std::to_string(stats.stolen_voices));
    add_field(data, "rejected_voices", std::to_string(stats.rejected_voices));
    add_field(data, "fallback_voices", std::to_string(stats.fallback_voices));
    add_field(data, "fallback_diagnostics", std::to_string(stats.fallback_diagnostics));
    add_field(data, "device_reinitializations", std::to_string(stats.device_reinitializations));
    add_field(data, "device_failures", std::to_string(stats.device_failures));
    add_field(data, "cached_assets", std::to_string(stats.cached_assets));
    add_field(data, "asset_cache_hits", std::to_string(stats.asset_cache_hits));
    add_field(data, "source_asset_loads", std::to_string(stats.source_asset_loads));
    add_field(data, "master_gain", std::to_string(stats.master_gain));
    if (stats.active_voices > stats.maximum_voices) {
        data.state = "invalid";
        add_issue(data, InspectionSeverity::error, "audio_stats.voice_limit_exceeded",
                  "active audio voices exceed the configured maximum");
    }
    return data;
}

InspectionData Inspector::inspect(const content::ContentValidationReport& report) {
    InspectionData data;
    data.object_type = "content_validation_report";
    data.display_name = "Content Validation Report";

    const auto warning_count = report.count_severity(modding::DiagnosticSeverity::warning);
    const auto error_count = report.count_severity(modding::DiagnosticSeverity::error);

    add_field(data, "mod_count", std::to_string(report.mods.size()));
    add_field(data, "resource_pack_count", std::to_string(report.resource_packs.size()));
    add_field(data, "resource_pack_plan_count",
              std::to_string(report.resource_pack_load_plan.size()));
    add_field(data, "prototype_count", std::to_string(report.prototypes.size()));
    add_field(data, "prototype_patch_count", std::to_string(report.prototype_patches.size()));
    add_field(data, "applied_patch_count", std::to_string(report.applied_patch_count));
    add_field(data, "mod_fingerprint_count", std::to_string(report.mod_fingerprints.size()));
    add_field(data, "script_module_count", std::to_string(report.script_modules.size()));
    add_field(data, "asset_record_count", std::to_string(report.asset_catalog.record_count()));
    add_field(data, "active_asset_count", std::to_string(report.asset_catalog.active_count()));
    add_field(data, "item_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::item)));
    add_field(data, "cargo_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::cargo)));
    add_field(data, "entity_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::entity)));
    add_field(data, "voxel_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::voxel)));
    add_field(data, "build_piece_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::build_piece)));
    add_field(data, "assembly_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::assembly)));
    add_field(data, "workpiece_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::workpiece)));
    add_field(data, "process_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::process)));
    add_field(data, "room_descriptor_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::room_descriptor)));
    add_field(data, "material_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::material)));
    add_field(data, "scenario_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::scenario)));
    add_field(data, "sound_event_prototype_count",
              std::to_string(report.count_kind(modding::PrototypeKinds::sound_event)));
    add_field(data, "item_definition_count", std::to_string(report.item_definitions.size()));
    add_field(data, "cargo_definition_count", std::to_string(report.cargo_definitions.size()));
    add_field(data, "entity_definition_count", std::to_string(report.entity_definitions.size()));
    add_field(data, "voxel_palette_count", std::to_string(report.voxel_palette.size()));
    add_field(data, "assembly_definition_count",
              std::to_string(report.assembly_definitions.size()));
    add_field(data, "process_definition_count", std::to_string(report.process_definitions.size()));
    add_field(data, "room_descriptor_definition_count",
              std::to_string(report.room_descriptor_definitions.size()));
    add_field(data, "workpiece_definition_count",
              std::to_string(report.workpiece_definitions.size()));
    add_field(data, "scenario_definition_count",
              std::to_string(report.scenario_definitions.size()));
    add_field(data, "material_definition_count", std::to_string(report.material_registry.size()));
    add_field(data, "material_asset_reference_count",
              std::to_string(report.material_assets.references.size()));
    add_field(data, "material_asset_override_count",
              std::to_string(report.material_assets.override_count()));
    add_field(data, "sound_event_count", std::to_string(report.sound_events.size()));
    add_field(data, "warning_count", std::to_string(warning_count));
    add_field(data, "error_count", std::to_string(error_count));

    if (!report.mods.empty()) {
        add_field(data, "first_mod_id", report.mods.front().id);
    }
    if (!report.resource_pack_load_plan.entries.empty()) {
        const auto& first_pack = report.resource_pack_load_plan.entries.front();
        const auto& last_pack = report.resource_pack_load_plan.entries.back();
        add_field(data, "first_resource_pack_id", first_pack.manifest.id);
        add_field(data, "first_resource_pack_priority", std::to_string(first_pack.asset_priority));
        add_field(data, "last_resource_pack_id", last_pack.manifest.id);
        add_field(data, "last_resource_pack_priority", std::to_string(last_pack.asset_priority));
    }
    if (!report.diagnostics.empty()) {
        add_field(data, "first_diagnostic_code", report.diagnostics.front().code);
        add_field(data, "first_diagnostic_message", report.diagnostics.front().message);
    }

    auto pack_plan_inspection = inspect(report.resource_pack_load_plan);
    for (const auto& issue : pack_plan_inspection.issues) {
        if (issue.severity == InspectionSeverity::error) {
            add_issue(data, issue.severity, issue.code, issue.message);
        }
    }
    for (const auto& diagnostic : report.diagnostics) {
        add_issue(data, inspection_severity(diagnostic.severity), diagnostic.code,
                  diagnostic.message);
    }

    if (data.has_errors()) {
        data.state = "invalid";
    } else if (report.mods.empty() && report.prototypes.empty() &&
               report.asset_catalog.record_count() == 0) {
        data.state = "empty";
    } else if (warning_count > 0) {
        data.state = "warnings";
    } else {
        data.state = "ready";
    }
    return data;
}

InspectionData Inspector::inspect(const assets::CookedAssetRecord& record) {
    InspectionData data;
    data.object_type = "cooked_asset_record";
    data.display_name = record.logical_id;
    data.runtime_id = record.logical_id;
    data.state = record.active ? "active" : "inactive";
    add_field(data, "logical_id", record.logical_id);
    add_field(data, "kind", std::string(assets::asset_kind_name(record.kind)));
    add_field(data, "source_virtual_path", virtual_path_text(record.source_virtual_path));
    add_field(data, "source_namespace", record.source_virtual_path.namespace_id);
    add_field(data, "source_relative_path",
              record.source_virtual_path.relative_path.generic_string());
    add_field(data, "source_kind", std::string(assets::asset_source_kind_name(record.source_kind)));
    add_field(data, "source_id", record.source_id);
    add_field(data, "source_hash", record.source_hash);
    add_field(data, "dependency_hash", record.dependency_hash);
    add_field(data, "pipeline_id", record.pipeline_id);
    add_field(data, "cooked_relative_path", record.cooked_relative_path.generic_string());
    add_field(data, "cooked_hash", record.cooked_hash);
    add_field(data, "pipeline_version", std::to_string(record.pipeline_version));
    add_field(data, "active", bool_text(record.active));
    add_field(data, "dependency_count", std::to_string(record.dependencies.size()));
    if (!record.dependencies.empty()) {
        add_field(data, "first_dependency", virtual_path_text(record.dependencies.front()));
    }

    if (!core::PrototypeId::parse(record.logical_id)) {
        add_issue(data, InspectionSeverity::error, "cooked_asset_record.invalid_logical_id",
                  "cooked asset logical id is not a safe relative id");
    }
    if (!core::is_valid_namespace_id(record.source_virtual_path.namespace_id) ||
        !is_valid_relative_path(record.source_virtual_path.relative_path)) {
        add_issue(data, InspectionSeverity::error, "cooked_asset_record.invalid_source_path",
                  "cooked asset source virtual path is invalid");
    }
    if (record.logical_id != assets::asset_logical_id(record.source_virtual_path)) {
        add_issue(data, InspectionSeverity::warning, "cooked_asset_record.path_mismatch",
                  "cooked asset logical id differs from its source virtual relative path");
    }
    if (!core::is_valid_namespace_id(record.source_id)) {
        add_issue(data, InspectionSeverity::error, "cooked_asset_record.invalid_source_id",
                  "cooked asset source id is invalid");
    }
    if (record.source_hash.empty()) {
        add_issue(data, InspectionSeverity::error, "cooked_asset_record.missing_source_hash",
                  "cooked asset source hash is missing");
    }
    if (record.dependency_hash.empty()) {
        add_issue(data, InspectionSeverity::error, "cooked_asset_record.missing_dependency_hash",
                  "cooked asset dependency hash is missing");
    }
    if (record.pipeline_id.empty()) {
        add_issue(data, InspectionSeverity::error, "cooked_asset_record.missing_pipeline_id",
                  "cooked asset pipeline identity is missing");
    }
    if (!is_valid_relative_path(record.cooked_relative_path)) {
        add_issue(data, InspectionSeverity::error, "cooked_asset_record.invalid_cooked_path",
                  "cooked asset output path is invalid");
    }
    if (record.cooked_hash.empty()) {
        add_issue(data, InspectionSeverity::error, "cooked_asset_record.missing_cooked_hash",
                  "cooked asset payload hash is missing");
    }
    if (record.pipeline_version == 0) {
        add_issue(data, InspectionSeverity::error, "cooked_asset_record.invalid_pipeline_version",
                  "cooked asset pipeline version must be non-zero");
    }
    for (const auto& dependency : record.dependencies) {
        if (!core::is_valid_namespace_id(dependency.namespace_id) ||
            !is_valid_relative_path(dependency.relative_path)) {
            add_issue(data, InspectionSeverity::error, "cooked_asset_record.invalid_dependency",
                      "cooked asset dependency virtual path is invalid");
            break;
        }
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const assets::CookedAssetManifest& manifest) {
    InspectionData data;
    data.object_type = "cooked_asset_manifest";
    data.display_name = "Cooked Asset Manifest";

    std::size_t invalid_record_count = 0;
    std::size_t warning_record_count = 0;
    std::size_t active_texture_count = 0;
    std::size_t active_model_count = 0;
    std::size_t active_shader_count = 0;
    for (const auto& record : manifest.records) {
        auto record_inspection = inspect(record);
        if (record_inspection.has_errors()) {
            ++invalid_record_count;
        }
        if (!record_inspection.issues.empty()) {
            ++warning_record_count;
        }
        if (!record.active) {
            continue;
        }
        switch (record.kind) {
        case assets::AssetKind::texture:
            ++active_texture_count;
            break;
        case assets::AssetKind::model:
            ++active_model_count;
            break;
        case assets::AssetKind::shader:
            ++active_shader_count;
            break;
        default:
            break;
        }
    }

    const auto dependencies = manifest.dependency_report();
    add_field(data, "schema_version", std::to_string(manifest.schema_version));
    add_field(data, "profile", manifest.profile);
    add_field(data, "record_count", std::to_string(manifest.records.size()));
    add_field(data, "active_count", std::to_string(manifest.active_count()));
    add_field(data, "inactive_count",
              std::to_string(manifest.records.size() - manifest.active_count()));
    add_field(data, "texture_count",
              std::to_string(manifest.count_kind(assets::AssetKind::texture)));
    add_field(data, "model_count", std::to_string(manifest.count_kind(assets::AssetKind::model)));
    add_field(data, "shader_count", std::to_string(manifest.count_kind(assets::AssetKind::shader)));
    add_field(data, "sound_count", std::to_string(manifest.count_kind(assets::AssetKind::sound)));
    add_field(data, "music_count", std::to_string(manifest.count_kind(assets::AssetKind::music)));
    add_field(data, "font_count", std::to_string(manifest.count_kind(assets::AssetKind::font)));
    add_field(data, "data_count", std::to_string(manifest.count_kind(assets::AssetKind::data)));
    add_field(data, "active_texture_count", std::to_string(active_texture_count));
    add_field(data, "active_model_count", std::to_string(active_model_count));
    add_field(data, "active_shader_count", std::to_string(active_shader_count));
    add_field(data, "invalid_record_count", std::to_string(invalid_record_count));
    add_field(data, "record_issue_count", std::to_string(warning_record_count));
    add_field(data, "dependency_issue_count", std::to_string(dependencies.issues.size()));

    if (!manifest.records.empty()) {
        add_field(data, "first_record_logical_id", manifest.records.front().logical_id);
        if (const auto* active = manifest.find_active(manifest.records.front().logical_id)) {
            add_field(data, "first_record_active", bool_text(active->active));
        }
    }
    if (!dependencies.issues.empty()) {
        add_field(data, "first_dependency_issue_logical_id",
                  dependencies.issues.front().logical_id);
        add_field(data, "first_dependency_issue_path",
                  virtual_path_text(dependencies.issues.front().dependency));
    }

    add_status_issue(data, manifest.validate());
    for (const auto& issue : dependencies.issues) {
        add_issue(data, InspectionSeverity::error, issue.code, issue.message);
    }

    if (data.has_errors()) {
        data.state = "invalid";
    } else if (manifest.records.empty()) {
        data.state = "empty";
        add_issue(data, InspectionSeverity::warning, "cooked_asset_manifest.empty",
                  "cooked asset manifest has no records");
    } else if (warning_record_count > 0) {
        data.state = "warnings";
    } else {
        data.state = "ready";
    }
    return data;
}

InspectionData Inspector::inspect(const assets::CookedAssetStore& store) {
    InspectionData data;
    data.object_type = "cooked_asset_store";
    data.display_name = "Cooked Asset Store";
    add_field(data, "root", store.root().generic_string());
    add_field(data, "profile", store.manifest().profile);

    auto manifest_inspection = inspect(store.manifest());
    for (const auto& field : manifest_inspection.fields) {
        add_field(data, "manifest_" + field.name, field.value);
    }
    for (const auto& issue : manifest_inspection.issues) {
        add_issue(data, issue.severity, issue.code, issue.message);
    }

    if (data.has_errors()) {
        data.state = "invalid";
    } else if (store.root().empty()) {
        data.state = "invalid";
        add_issue(data, InspectionSeverity::error, "cooked_asset_store.missing_root",
                  "cooked asset store root is empty");
    } else if (store.manifest().records.empty()) {
        data.state = "empty";
    } else {
        data.state = "loaded";
    }
    return data;
}

InspectionData Inspector::inspect(const assets::AssetCookBackendInfo& backend) {
    InspectionData data;
    data.object_type = "asset_cook_backend";
    data.display_name = "Asset Cook Backend " + std::string(backend.name);
    data.state = backend.available ? "available" : "unavailable";
    add_field(data, "backend", std::string(backend.name));
    add_field(data, "available", bool_text(backend.available));
    add_field(data, "status", std::string(backend.status));
    if (!backend.available) {
        add_issue(data, InspectionSeverity::warning, "asset_cooker.backend_unavailable",
                  std::string(backend.status));
    }
    return data;
}

InspectionData Inspector::inspect(const assets::AssetCookPipelineInfo& pipeline) {
    InspectionData data;
    data.object_type = "asset_cook_pipeline";
    data.display_name = "Asset Cook Pipeline " + std::string(pipeline.name);
    data.state = pipeline.available ? "available" : "unavailable";
    add_field(data, "kind", std::string(assets::asset_kind_name(pipeline.kind)));
    add_field(data, "backend", std::string(assets::asset_cook_backend_name(pipeline.backend)));
    add_field(data, "pipeline", std::string(pipeline.name));
    add_field(data, "available", bool_text(pipeline.available));
    add_field(data, "converts_source_format", bool_text(pipeline.converts_source_format));
    add_field(data, "status", std::string(pipeline.status));
    if (!pipeline.available) {
        add_issue(data, InspectionSeverity::warning, "asset_cooker.pipeline_unavailable",
                  std::string(pipeline.status));
    }
    return data;
}

InspectionData Inspector::inspect(const renderer::shaders::CompiledShaderRecord& record) {
    InspectionData data;
    data.object_type = "compiled_shader_record";
    data.display_name = record.logical_id;
    data.runtime_id = record.logical_id;
    data.state = "compiled";
    add_field(data, "logical_id", record.logical_id);
    add_field(data, "source_virtual_path", virtual_path_text(record.source_virtual_path));
    add_field(data, "source_namespace", record.source_virtual_path.namespace_id);
    add_field(data, "source_relative_path",
              record.source_virtual_path.relative_path.generic_string());
    add_field(data, "source_kind", std::string(assets::asset_source_kind_name(record.source_kind)));
    add_field(data, "source_id", record.source_id);
    add_field(data, "source_hash", record.source_hash);
    add_field(data, "language",
              std::string(renderer::shaders::shader_source_language_name(record.language)));
    add_field(data, "role", std::string(renderer::shaders::shader_source_role_name(record.role)));
    add_field(data, "compiled_relative_path", record.compiled_relative_path.generic_string());
    add_field(data, "compiled_hash", record.compiled_hash);
    add_field(data, "backend", record.backend);
    add_field(data, "pipeline_version", std::to_string(record.pipeline_version));
    add_field(data, "source_bytes", std::to_string(record.source_bytes));

    if (!core::PrototypeId::parse(record.logical_id)) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.invalid_logical_id",
                  "compiled shader logical id is not a safe relative id");
    }
    if (!core::is_valid_namespace_id(record.source_virtual_path.namespace_id) ||
        !is_valid_relative_path(record.source_virtual_path.relative_path)) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.invalid_source_path",
                  "compiled shader source virtual path is invalid");
    }
    if (record.logical_id != assets::asset_logical_id(record.source_virtual_path)) {
        add_issue(data, InspectionSeverity::warning, "compiled_shader.path_mismatch",
                  "compiled shader logical id differs from its source virtual relative path");
    }
    if (!core::is_valid_namespace_id(record.source_id)) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.invalid_source_id",
                  "compiled shader source id is invalid");
    }
    if (record.source_hash.empty()) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.missing_source_hash",
                  "compiled shader source hash is missing");
    }
    if (record.language == renderer::shaders::ShaderSourceLanguage::unknown) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.unknown_language",
                  "compiled shader source language is unknown");
    }
    if (record.role == renderer::shaders::ShaderSourceRole::unknown) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.unknown_role",
                  "compiled shader source role is unknown");
    }
    if (!is_valid_relative_path(record.compiled_relative_path)) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.invalid_compiled_path",
                  "compiled shader output path is invalid");
    }
    if (record.compiled_hash.empty()) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.missing_compiled_hash",
                  "compiled shader hash is missing");
    }
    if (record.backend.empty()) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.missing_backend",
                  "compiled shader backend is missing");
    }
    if (record.pipeline_version == 0) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.invalid_pipeline_version",
                  "compiled shader pipeline version must be non-zero");
    }
    if (record.source_bytes == 0) {
        add_issue(data, InspectionSeverity::error, "compiled_shader.empty_source",
                  "compiled shader source byte count must be non-zero");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const renderer::shaders::ShaderCompileResult& result) {
    InspectionData data;
    data.object_type = "shader_compile_result";
    data.display_name = "Shader Compile Result";

    std::size_t invalid_record_count = 0;
    std::size_t warning_record_count = 0;
    std::size_t slang_count = 0;
    std::size_t hlsl_count = 0;
    std::size_t spirv_count = 0;
    std::size_t template_count = 0;
    std::size_t vertex_count = 0;
    std::size_t fragment_count = 0;
    std::size_t compute_count = 0;
    std::size_t library_count = 0;

    for (const auto& record : result.records) {
        auto record_inspection = inspect(record);
        if (record_inspection.has_errors()) {
            ++invalid_record_count;
        }
        if (!record_inspection.issues.empty()) {
            ++warning_record_count;
        }
        switch (record.language) {
        case renderer::shaders::ShaderSourceLanguage::slang:
            ++slang_count;
            break;
        case renderer::shaders::ShaderSourceLanguage::hlsl:
            ++hlsl_count;
            break;
        case renderer::shaders::ShaderSourceLanguage::spirv:
            ++spirv_count;
            break;
        case renderer::shaders::ShaderSourceLanguage::unknown:
            break;
        }
        switch (record.role) {
        case renderer::shaders::ShaderSourceRole::template_source:
            ++template_count;
            break;
        case renderer::shaders::ShaderSourceRole::vertex:
            ++vertex_count;
            break;
        case renderer::shaders::ShaderSourceRole::fragment:
            ++fragment_count;
            break;
        case renderer::shaders::ShaderSourceRole::compute:
            ++compute_count;
            break;
        case renderer::shaders::ShaderSourceRole::library:
            ++library_count;
            break;
        case renderer::shaders::ShaderSourceRole::unknown:
            break;
        }
    }

    add_field(data, "manifest_path", result.manifest_path.generic_string());
    add_field(data, "record_count", std::to_string(result.records.size()));
    add_field(data, "compiled_shader_count", std::to_string(result.compiled_shader_count));
    add_field(data, "compiled_payload_bytes", std::to_string(result.compiled_payload_bytes));
    add_field(data, "diagnostic_count", std::to_string(result.diagnostics.size()));
    add_field(data, "invalid_record_count", std::to_string(invalid_record_count));
    add_field(data, "record_issue_count", std::to_string(warning_record_count));
    add_field(data, "slang_count", std::to_string(slang_count));
    add_field(data, "hlsl_count", std::to_string(hlsl_count));
    add_field(data, "spirv_count", std::to_string(spirv_count));
    add_field(data, "template_count", std::to_string(template_count));
    add_field(data, "vertex_count", std::to_string(vertex_count));
    add_field(data, "fragment_count", std::to_string(fragment_count));
    add_field(data, "compute_count", std::to_string(compute_count));
    add_field(data, "library_count", std::to_string(library_count));
    if (!result.records.empty()) {
        add_field(data, "first_record_logical_id", result.records.front().logical_id);
        add_field(data, "first_record_backend", result.records.front().backend);
    }
    if (!result.diagnostics.empty()) {
        add_field(data, "first_diagnostic_code", result.diagnostics.front().code);
        add_field(data, "first_diagnostic_message", result.diagnostics.front().message);
    }

    if (result.manifest_path.empty()) {
        add_issue(data, InspectionSeverity::error, "shader_compile_result.missing_manifest_path",
                  "shader compile result has no manifest path");
    }
    if (!result.has_errors() && result.compiled_shader_count != result.records.size()) {
        add_issue(data, InspectionSeverity::error, "shader_compile_result.count_mismatch",
                  "compiled shader count does not match record count");
    }
    if (!result.has_errors() && result.compiled_shader_count > 0 &&
        result.compiled_payload_bytes == 0) {
        add_issue(data, InspectionSeverity::error, "shader_compile_result.empty_payloads",
                  "compiled shader payload byte count is zero");
    }
    if (invalid_record_count > 0) {
        add_issue(data, InspectionSeverity::error, "shader_compile_result.invalid_records",
                  "shader compile result contains invalid records");
    } else if (warning_record_count > 0) {
        add_issue(data, InspectionSeverity::warning, "shader_compile_result.record_warnings",
                  "shader compile result contains record warnings");
    }
    for (const auto& diagnostic : result.diagnostics) {
        add_issue(data, inspection_severity(diagnostic.severity), diagnostic.code,
                  diagnostic.message);
    }

    if (data.has_errors()) {
        data.state = "invalid";
    } else if (result.records.empty()) {
        data.state = "empty";
    } else {
        data.state = "compiled";
    }
    return data;
}

InspectionData Inspector::inspect(const modding::ModLifecyclePlan& plan) {
    InspectionData data;
    data.object_type = "mod_lifecycle_plan";
    data.display_name = "Mod Lifecycle Plan";
    data.state = plan.has_errors() ? "invalid" : "valid";
    add_field(data, "task_count", std::to_string(plan.tasks.size()));
    add_field(data, "diagnostic_count", std::to_string(plan.diagnostics.size()));

    constexpr std::array stages{
        modding::ModLifecycleStage::settings,       modding::ModLifecycleStage::prototypes,
        modding::ModLifecycleStage::data_updates,   modding::ModLifecycleStage::final_fixes,
        modding::ModLifecycleStage::assets,         modding::ModLifecycleStage::migration,
        modding::ModLifecycleStage::runtime_server, modding::ModLifecycleStage::runtime_client,
    };
    for (const auto stage : stages) {
        add_field(data, "stage_" + std::string(modding::mod_lifecycle_stage_name(stage)) + "_count",
                  std::to_string(plan.count_stage(stage)));
    }

    for (std::size_t index = 0; index < plan.tasks.size(); ++index) {
        const auto& task = plan.tasks[index];
        const auto prefix = "task_" + std::to_string(index) + '_';
        add_field(data, prefix + "stage",
                  std::string(modding::mod_lifecycle_stage_name(task.stage)));
        add_field(data, prefix + "kind",
                  std::string(modding::mod_lifecycle_task_kind_name(task.kind)));
        add_field(data, prefix + "mod", task.mod_id);
        add_field(data, prefix + "source", task.source.generic_string());
    }

    for (const auto& diagnostic : plan.diagnostics) {
        add_issue(data, inspection_severity(diagnostic.severity), diagnostic.code,
                  diagnostic.message + " (" + diagnostic.source.generic_string() + ")");
    }

    return data;
}

InspectionData Inspector::inspect(const scripting::ScriptBackendInfo& backend) {
    InspectionData data;
    data.object_type = "script_backend";
    data.display_name = "Script Backend " + std::string(backend.name);
    data.state = backend.available ? "available" : "unavailable";
    add_field(data, "backend", std::string(scripting::script_backend_name(backend.backend)));
    add_field(data, "available", bool_text(backend.available));
    add_field(data, "status", std::string(backend.status));
    if (!backend.available) {
        add_issue(data, InspectionSeverity::warning, "scripting.backend_unavailable",
                  std::string(backend.status));
    }
    return data;
}

InspectionData Inspector::inspect(const scripting::ScriptModuleDesc& module) {
    InspectionData data;
    data.object_type = "script_module";
    data.display_name = module.module_id;
    data.runtime_id = module.module_id;
    data.state = std::string(scripting::script_stage_name(module.stage));
    add_field(data, "source_mod_id", module.source_mod_id);
    add_field(data, "source_path", module.source_path.generic_string());
    add_field(data, "stage", std::string(scripting::script_stage_name(module.stage)));
    add_field(data, "api_version", std::to_string(module.api_version));
    add_field(data, "source_bytes", std::to_string(module.source.size()));
    add_field(data, "permission_count", std::to_string(module.permissions.size()));
    add_field(data, "permissions", script_permission_list(module.permissions));

    add_status_issue(data, scripting::validate_script_module_desc(
                               module, source_limit_for_inspection(module.source.size())));
    return data;
}

InspectionData Inspector::inspect(const scripting::ScriptRuntimeStats& stats) {
    InspectionData data;
    data.object_type = "script_runtime_stats";
    data.display_name = "Script runtime";
    add_field(data, "vm_count", std::to_string(stats.vm_count));
    add_field(data, "module_count", std::to_string(stats.module_count));
    add_field(data, "current_memory_bytes", std::to_string(stats.current_memory_bytes));
    add_field(data, "peak_memory_bytes", std::to_string(stats.peak_memory_bytes));
    add_field(data, "memory_limit_bytes_per_vm", std::to_string(stats.memory_limit_bytes_per_vm));
    add_field(data, "compiled_source_bytes", std::to_string(stats.compiled_source_bytes));
    add_field(data, "compiled_bytecode_bytes", std::to_string(stats.compiled_bytecode_bytes));
    add_field(data, "call_count", std::to_string(stats.call_count));
    add_field(data, "failed_call_count", std::to_string(stats.failed_call_count));
    add_field(data, "interrupt_count", std::to_string(stats.interrupt_count));
    add_field(data, "emitted_event_count", std::to_string(stats.emitted_event_count));
    add_field(data, "total_call_microseconds", std::to_string(stats.total_call_microseconds));
    add_field(data, "last_call_microseconds", std::to_string(stats.last_call_microseconds));
    return data;
}

InspectionData Inspector::inspect(const scripting::ScriptHostApiDesc& host_api) {
    InspectionData data;
    data.object_type = "script_host_api";
    data.display_name = host_api.api_id;
    data.runtime_id = host_api.api_id;
    data.state = std::string(scripting::script_stage_name(host_api.stage));
    add_field(data, "api_id", host_api.api_id);
    add_field(data, "stage", std::string(scripting::script_stage_name(host_api.stage)));
    add_field(data, "min_module_api_version", std::to_string(host_api.min_module_api_version));
    add_field(data, "required_permission_count",
              std::to_string(host_api.required_permissions.size()));
    add_field(data, "required_permissions", script_permission_list(host_api.required_permissions));
    add_field(data, "argument_count", std::to_string(host_api.arguments.size()));
    add_field(data, "arguments", script_host_api_argument_list(host_api.arguments));
    add_status_issue(data, scripting::validate_script_host_api_desc(host_api));
    return data;
}

InspectionData Inspector::inspect(const scripting::ScriptHostEvent& event) {
    InspectionData data;
    data.object_type = "script_host_event";
    data.display_name = event.api_id;
    data.runtime_id = std::to_string(event.sequence);
    data.state = std::string(scripting::script_stage_name(event.stage));
    add_field(data, "sequence", std::to_string(event.sequence));
    add_field(data, "api_id", event.api_id);
    add_field(data, "module_id", event.module_id);
    add_field(data, "source_mod_id", event.source_mod_id);
    add_field(data, "source_path", event.source_path.generic_string());
    add_field(data, "stage", std::string(scripting::script_stage_name(event.stage)));
    add_field(data, "function_name", event.function_name);
    add_field(data, "module_api_version", std::to_string(event.module_api_version));
    add_field(data, "consumed_instruction_estimate",
              std::to_string(event.consumed_instruction_estimate));
    add_field(data, "argument_count", std::to_string(event.arguments.size()));
    add_status_issue(data, scripting::validate_script_host_event(event));
    return data;
}

InspectionData Inspector::inspect(const scripting::ScriptHostEventBatch& batch) {
    InspectionData data;
    data.object_type = "script_host_event_batch";
    data.display_name = "Script Host Event Batch";
    data.runtime_id = batch.events.empty() ? "" : std::to_string(batch.first_sequence);
    data.state = batch.events.empty() ? "empty" : "queued";
    add_field(data, "first_sequence", std::to_string(batch.first_sequence));
    add_field(data, "last_sequence", std::to_string(batch.last_sequence));
    add_field(data, "event_count", std::to_string(batch.events.size()));
    add_status_issue(data, scripting::validate_script_host_event_batch(batch));
    return data;
}

InspectionData Inspector::inspect(const simulation::SimulationLodPolicy& policy) {
    InspectionData data;
    data.object_type = "simulation_lod_policy";
    data.display_name = "Simulation LOD Policy";
    add_field(data, "full_radius", std::to_string(policy.full_radius));
    add_field(data, "simplified_radius", std::to_string(policy.simplified_radius));
    add_field(data, "full_tick_interval_ms", std::to_string(policy.full_tick_interval_ms));
    add_field(data, "simplified_tick_interval_ms",
              std::to_string(policy.simplified_tick_interval_ms));
    add_field(data, "sleeping_tick_interval_ms", std::to_string(policy.sleeping_tick_interval_ms));
    add_status_issue(data, policy.validate());
    data.state = data.has_errors() ? "invalid" : "valid";
    return data;
}

InspectionData Inspector::inspect(const simulation::SimulationTickBudget& budget) {
    InspectionData data;
    data.object_type = "simulation_tick_budget";
    data.display_name = "Simulation Tick Budget";
    add_field(data, "maximum_updates_per_tick", std::to_string(budget.maximum_updates_per_tick));
    add_field(data, "maximum_work_units_per_tick",
              std::to_string(budget.maximum_work_units_per_tick));
    add_field(data, "maximum_full_catch_up_delta_ms_per_update",
              std::to_string(budget.maximum_full_catch_up_delta_ms_per_update));
    add_field(data, "maximum_simplified_catch_up_delta_ms_per_update",
              std::to_string(budget.maximum_simplified_catch_up_delta_ms_per_update));
    add_field(data, "maximum_sleeping_catch_up_delta_ms_per_update",
              std::to_string(budget.maximum_sleeping_catch_up_delta_ms_per_update));
    add_field(data, "maximum_catch_up_delta_ms_per_tick",
              std::to_string(budget.maximum_catch_up_delta_ms_per_tick));
    add_status_issue(data, budget.validate());
    data.state = data.has_errors() ? "invalid" : "valid";
    return data;
}

InspectionData Inspector::inspect(const simulation::SimulationSubject& subject) {
    InspectionData data;
    data.object_type = "simulation_subject";
    data.display_name = subject.label.empty() ? "Simulation Subject" : subject.label;
    data.prototype_id = prototype_text(subject.prototype_id);
    data.save_id = save_id_text(subject.save_id);
    data.runtime_id = runtime_handle_text(subject.runtime_handle);

    if (subject.forced_lod.has_value()) {
        data.state = "forced_" + std::string(simulation::to_string(subject.forced_lod.value()));
    } else if (subject.sleeping) {
        data.state = "sleeping";
    } else {
        data.state = subject.persistent ? "persistent" : "runtime";
    }

    add_field(data, "kind", std::string(simulation::to_string(subject.kind)));
    add_field(data, "label", subject.label);
    add_field(data, "process_id", process_id_text(subject.process_id));
    add_field(data, "coord_x", std::to_string(subject.coord.x));
    add_field(data, "coord_y", std::to_string(subject.coord.y));
    add_field(data, "coord_z", std::to_string(subject.coord.z));
    add_field(data, "last_update_time_ms", std::to_string(subject.last_update_time_ms));
    add_field(data, "estimated_update_work_units",
              std::to_string(subject.estimated_update_work_units));
    add_field(data, "persistent", bool_text(subject.persistent));
    add_field(data, "sleeping", bool_text(subject.sleeping));
    add_field(data, "forced_lod",
              subject.forced_lod.has_value()
                  ? std::string(simulation::to_string(subject.forced_lod.value()))
                  : "");

    if (subject.persistent && !subject.save_id.is_valid()) {
        add_issue(data, InspectionSeverity::error, "simulation.missing_save_id",
                  "persistent simulation subjects need stable save ids");
    }
    if (!subject.persistent && subject.save_id.is_valid()) {
        add_issue(data, InspectionSeverity::error, "simulation.unexpected_save_id",
                  "non-persistent simulation subjects must not claim a save id");
    }
    if (subject.kind == simulation::SimulationSubjectKind::process_owner &&
        !subject.process_id.is_valid()) {
        add_issue(data, InspectionSeverity::error, "simulation.missing_process_id",
                  "process-owner simulation subjects need stable process ids");
    }
    if (!subject.persistent && !subject.runtime_handle.is_valid()) {
        add_issue(data, InspectionSeverity::error, "simulation.missing_runtime_handle",
                  "non-persistent simulation subjects need stable runtime handles");
    }
    if (subject.estimated_update_work_units == 0) {
        add_issue(data, InspectionSeverity::error, "simulation.invalid_update_work",
                  "simulation update work estimates must be positive");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const simulation::SimulationLodDecision& decision) {
    InspectionData data;
    data.object_type = "simulation_lod_decision";
    data.display_name = "Simulation LOD Decision";
    data.save_id = save_id_text(decision.save_id);
    data.runtime_id = runtime_handle_text(decision.runtime_handle);
    data.state = std::string(simulation::to_string(decision.lod));
    add_field(data, "kind", std::string(simulation::to_string(decision.kind)));
    add_field(data, "process_id", process_id_text(decision.process_id));
    add_field(data, "lod", std::string(simulation::to_string(decision.lod)));
    add_field(data, "nearest_viewer_distance_squared",
              std::to_string(decision.nearest_viewer_distance_squared));
    add_field(data, "elapsed_since_update_ms", std::to_string(decision.elapsed_since_update_ms));
    add_field(data, "offline_delta_ms", std::to_string(decision.offline_delta_ms));
    add_field(data, "tick_interval_ms", std::to_string(decision.tick_interval_ms));
    add_field(data, "estimated_update_work_units",
              std::to_string(decision.estimated_update_work_units));
    add_field(data, "due_for_tick", bool_text(decision.due_for_tick));

    if (decision.lod == simulation::SimulationLod::unloaded && decision.offline_delta_ms > 0) {
        add_issue(data, InspectionSeverity::info, "simulation.unloaded_offline_delta",
                  "unloaded subject has timestamp delta to apply on reload");
    }
    if (decision.lod != simulation::SimulationLod::unloaded && decision.offline_delta_ms != 0) {
        add_issue(data, InspectionSeverity::warning, "simulation.loaded_offline_delta",
                  "loaded simulation decision should not carry offline delta");
    }
    return data;
}

InspectionData Inspector::inspect(const simulation::SimulationFramePlan& plan) {
    InspectionData data;
    data.object_type = "simulation_frame_plan";
    data.display_name = "Simulation Frame Plan";
    data.state = plan.decisions.empty() ? "empty" : plan.due_tick_count > 0 ? "active" : "idle";
    add_field(data, "decision_count", std::to_string(plan.decisions.size()));
    add_field(data, "full_count", std::to_string(plan.full_count));
    add_field(data, "simplified_count", std::to_string(plan.simplified_count));
    add_field(data, "sleeping_count", std::to_string(plan.sleeping_count));
    add_field(data, "unloaded_count", std::to_string(plan.unloaded_count));
    add_field(data, "due_tick_count", std::to_string(plan.due_tick_count));

    simulation::WorldTick total_offline_delta_ms = 0;
    for (const auto& decision : plan.decisions) {
        if (decision.offline_delta_ms > 0 &&
            total_offline_delta_ms <=
                std::numeric_limits<simulation::WorldTick>::max() - decision.offline_delta_ms) {
            total_offline_delta_ms += decision.offline_delta_ms;
        }
    }
    add_field(data, "total_offline_delta_ms", std::to_string(total_offline_delta_ms));

    if (!plan.decisions.empty()) {
        const auto& first = plan.decisions.front();
        add_field(data, "first_decision_save_id", save_id_text(first.save_id));
        add_field(data, "first_decision_runtime_handle", runtime_handle_text(first.runtime_handle));
        add_field(data, "first_decision_process_id", process_id_text(first.process_id));
        add_field(data, "first_decision_kind", std::string(simulation::to_string(first.kind)));
        add_field(data, "first_decision_lod", std::string(simulation::to_string(first.lod)));
        add_field(data, "first_decision_due_for_tick", bool_text(first.due_for_tick));
        add_field(data, "first_decision_offline_delta_ms", std::to_string(first.offline_delta_ms));
    } else {
        add_issue(data, InspectionSeverity::warning, "simulation.frame_plan_empty",
                  "simulation frame plan has no decisions");
    }

    add_simulation_frame_plan_issues(data, plan);
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const simulation::BudgetedSimulationFramePlan& plan) {
    InspectionData data;
    data.object_type = "budgeted_simulation_frame_plan";
    data.display_name = "Budgeted Simulation Frame Plan";
    data.state = plan.frame.decisions.empty() ? "empty"
                 : plan.budget_exhausted      ? "backlogged"
                 : plan.updates.empty()       ? "idle"
                                              : "scheduled";
    add_field(data, "now_ms", std::to_string(plan.now_ms));
    add_field(data, "decision_count", std::to_string(plan.frame.decisions.size()));
    add_field(data, "due_tick_count", std::to_string(plan.frame.due_tick_count));
    add_field(data, "scheduled_update_count", std::to_string(plan.updates.size()));
    add_field(data, "deferred_due_count", std::to_string(plan.deferred_due_count));
    add_field(data, "catch_up_due_count", std::to_string(plan.catch_up_due_count));
    add_field(data, "remaining_catch_up_count", std::to_string(plan.remaining_catch_up_count));
    add_field(data, "due_work_units", std::to_string(plan.due_work_units));
    add_field(data, "scheduled_work_units", std::to_string(plan.scheduled_work_units));
    add_field(data, "deferred_work_units", std::to_string(plan.deferred_work_units));
    add_field(data, "scheduled_delta_ms", std::to_string(plan.scheduled_delta_ms));
    add_field(data, "deferred_delta_ms", std::to_string(plan.deferred_delta_ms));
    add_field(data, "scheduled_catch_up_delta_ms",
              std::to_string(plan.scheduled_catch_up_delta_ms));
    add_field(data, "deferred_catch_up_delta_ms", std::to_string(plan.deferred_catch_up_delta_ms));
    add_field(data, "maximum_lateness_ms", std::to_string(plan.maximum_lateness_ms));
    add_field(data, "budget_exhausted", bool_text(plan.budget_exhausted));
    add_field(data, "counters_saturated", bool_text(plan.counters_saturated));
    add_budgeted_simulation_frame_plan_issues(data, plan);
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const net::TransportBackendInfo& backend) {
    InspectionData data;
    data.object_type = "transport_backend";
    data.display_name = "Transport Backend " + std::string(backend.name);
    data.state = backend.available ? "available" : "unavailable";
    add_field(data, "backend", std::string(net::transport_backend_name(backend.backend)));
    add_field(data, "available", bool_text(backend.available));
    add_field(data, "status", std::string(backend.status));
    if (!backend.available) {
        add_issue(data, InspectionSeverity::warning, "transport.backend_unavailable",
                  std::string(backend.status));
    }
    return data;
}

InspectionData Inspector::inspect(const net::TransportCapabilities& capabilities) {
    InspectionData data;
    data.object_type = "transport_capabilities";
    data.display_name = "Transport Capabilities";
    data.state = capabilities.supports_server_authoritative_host ? "host_capable" : "limited";
    add_field(data, "supports_reliable", bool_text(capabilities.supports_reliable));
    add_field(data, "supports_unreliable", bool_text(capabilities.supports_unreliable));
    add_field(data, "supports_server_authoritative_host",
              bool_text(capabilities.supports_server_authoritative_host));
    add_field(data, "enforces_reliable_command_order",
              bool_text(capabilities.enforces_reliable_command_order));
    add_field(data, "max_payload_bytes", std::to_string(capabilities.max_payload_bytes));
    add_field(data, "max_clients", std::to_string(capabilities.max_clients));

    if (!capabilities.supports_server_authoritative_host) {
        add_issue(data, InspectionSeverity::error, "transport.not_host_capable",
                  "transport backend cannot host an authoritative server session");
    }
    if (!capabilities.supports_reliable) {
        add_issue(data, InspectionSeverity::error, "transport.no_reliable_channel",
                  "transport backend must support reliable command delivery");
    }
    if (!capabilities.enforces_reliable_command_order) {
        add_issue(data, InspectionSeverity::warning, "transport.unordered_reliable_commands",
                  "transport backend does not enforce reliable command sequence order");
    }
    if (capabilities.max_payload_bytes == 0) {
        add_issue(data, InspectionSeverity::error, "transport.invalid_max_payload",
                  "transport max payload bytes must be non-zero");
    }
    if (capabilities.max_clients == 0) {
        add_issue(data, InspectionSeverity::error, "transport.invalid_max_clients",
                  "transport max client count must be non-zero");
    }
    return data;
}

InspectionData Inspector::inspect(const net::TransportServerWelcome& welcome) {
    InspectionData data;
    data.object_type = "transport_server_welcome";
    data.display_name = "Transport Server Welcome";
    data.save_id = "";
    data.runtime_id = net_id_text(welcome.assigned_client_id);
    data.state = welcome.protocol_version == net::transport_control_protocol_version
                     ? "current"
                     : "unsupported";
    add_field(data, "protocol_version", std::to_string(welcome.protocol_version));
    add_field(data, "server_id", net_id_text(welcome.server_id));
    add_field(data, "assigned_client_id", net_id_text(welcome.assigned_client_id));
    add_field(data, "max_payload_bytes", std::to_string(welcome.max_payload_bytes));
    add_field(data, "max_clients", std::to_string(welcome.max_clients));
    add_field(data, "supports_unreliable", bool_text(welcome.supports_unreliable));
    add_field(data, "enforces_reliable_command_order",
              bool_text(welcome.enforces_reliable_command_order));
    add_status_issue(data, net::validate_transport_server_welcome(welcome));
    return data;
}

InspectionData Inspector::inspect(const net::TransportClientSession& session) {
    InspectionData data;
    data.object_type = "transport_client_session";
    data.display_name = "Transport Client Session";
    data.save_id = "";
    data.runtime_id = net_id_text(session.client_id);
    data.state = session.protocol_version == net::transport_control_protocol_version
                     ? "connected"
                     : "unsupported";
    add_field(data, "protocol_version", std::to_string(session.protocol_version));
    add_field(data, "server_id", net_id_text(session.server_id));
    add_field(data, "client_id", net_id_text(session.client_id));
    add_field(data, "max_payload_bytes", std::to_string(session.max_payload_bytes));
    add_field(data, "max_clients", std::to_string(session.max_clients));
    add_field(data, "supports_unreliable", bool_text(session.supports_unreliable));
    add_field(data, "enforces_reliable_command_order",
              bool_text(session.enforces_reliable_command_order));
    add_field(data, "established_at_ms", std::to_string(session.established_at_ms));
    add_status_issue(data, net::validate_transport_client_session(session));
    return data;
}

InspectionData Inspector::inspect(const net::CommandOperationTrace& trace) {
    InspectionData data;
    data.object_type = "command_operation_trace";
    data.display_name = "Command Operation Trace";
    data.state = operation_trace_state(trace);
    add_operation_trace_fields(data, trace);
    add_operation_trace_issues(data, trace);
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const net::CommandDispatchResult& result) {
    InspectionData data;
    data.object_type = "command_dispatch_result";
    data.display_name = "Command Dispatch Result";
    data.runtime_id = std::to_string(result.sequence);
    data.state = result.committed_world_mutation ? "committed" : "accepted";
    add_field(data, "sequence", std::to_string(result.sequence));
    add_field(data, "command_type", result.command_type);
    add_field(data, "committed_world_mutation", bool_text(result.committed_world_mutation));
    add_field(data, "event_count", std::to_string(result.events.size()));
    add_field(data, "reserved_id_count", std::to_string(result.reserved_ids.size()));
    add_operation_trace_fields(data, result.operation_trace);
    add_operation_trace_issues(data, result.operation_trace);
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const net::CommandDispatchReport& report) {
    InspectionData data;
    data.object_type = "command_dispatch_report";
    data.display_name = "Command Dispatch Report";
    data.runtime_id = std::to_string(report.sequence);
    data.state = report.succeeded ? (report.committed_world_mutation ? "committed" : "accepted")
                                  : "rejected";
    add_field(data, "sequence", std::to_string(report.sequence));
    add_field(data, "command_type", report.command_type);
    add_field(data, "succeeded", bool_text(report.succeeded));
    add_field(data, "committed_world_mutation", bool_text(report.committed_world_mutation));
    add_field(data, "event_count", std::to_string(report.events.size()));
    add_field(data, "reserved_id_count", std::to_string(report.reserved_ids.size()));
    add_field(data, "error_code", report.error.has_value() ? report.error->code : "");
    add_field(data, "error_message", report.error.has_value() ? report.error->message : "");
    add_operation_trace_fields(data, report.operation_trace);
    add_operation_trace_issues(data, report.operation_trace);

    if (report.succeeded && report.error.has_value()) {
        add_issue(data, InspectionSeverity::error, "command_report.success_with_error",
                  "successful command dispatch report carries an error");
    }
    if (!report.succeeded && !report.error.has_value()) {
        add_issue(data, InspectionSeverity::error, "command_report.failure_without_error",
                  "failed command dispatch report has no error");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const net::HostSessionCommandReport& report) {
    InspectionData data;
    data.object_type = "host_session_command_report";
    data.display_name = "Host Session Command Report";
    data.runtime_id = std::to_string(report.sequence);
    data.state = report.success ? "accepted" : "rejected";
    add_field(data, "client_id", net_id_text(report.client_id));
    add_field(data, "sequence", std::to_string(report.sequence));
    add_field(data, "replication_sequence", std::to_string(report.replication_sequence));
    add_field(data, "command_type", report.command_type);
    add_field(data, "success", bool_text(report.success));
    add_field(data, "committed_world_mutation", bool_text(report.committed_world_mutation));
    add_field(data, "event_count", std::to_string(report.events.size()));
    add_field(data, "reserved_id_count", std::to_string(report.reserved_ids.size()));
    add_field(data, "error_code", report.error_code);
    add_field(data, "error_message", report.error_message);
    add_operation_trace_fields(data, report.operation_trace);
    if (report.success) {
        add_operation_trace_issues(data, report.operation_trace);
    }
    if (!report.success && report.error_code.empty()) {
        add_issue(data, InspectionSeverity::warning, "host_session_report.missing_error_code",
                  "rejected host session command report has no error code");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const net::HostSessionCommandResult& result) {
    InspectionData data;
    data.object_type = "host_session_command_result";
    data.display_name = "Host Session Command Result";
    data.runtime_id = std::to_string(result.sequence);
    data.state = result.success ? "accepted" : "rejected";
    add_field(data, "sequence", std::to_string(result.sequence));
    add_field(data, "command_type", result.command_type);
    add_field(data, "success", bool_text(result.success));
    add_field(data, "committed_world_mutation", bool_text(result.committed_world_mutation));
    add_field(data, "event_count", std::to_string(result.event_count));
    add_field(data, "reserved_id_count", std::to_string(result.reserved_id_count));
    add_field(data, "error_code", result.error_code);
    add_field(data, "error_message", result.error_message);
    add_status_issue(data, net::validate_host_session_command_result(result));
    return data;
}

InspectionData Inspector::inspect(const net::ReplicationBatch& batch) {
    InspectionData data;
    data.object_type = "replication_batch";
    data.display_name = "Replication Batch";
    data.runtime_id = std::to_string(net::replication_stream_sequence(batch));
    data.state = batch.events.empty() ? "empty" : "events";
    add_field(data, "command_sequence", std::to_string(batch.command_sequence));
    add_field(data, "replication_sequence",
              std::to_string(net::replication_stream_sequence(batch)));
    add_field(data, "source_client_id", net_id_text(batch.source_client_id));
    add_field(data, "command_type", batch.command_type);
    add_field(data, "event_count", std::to_string(batch.events.size()));
    add_field(data, "reserved_id_count", std::to_string(batch.reserved_ids.size()));
    if (!batch.events.empty()) {
        add_field(data, "first_event_type", batch.events.front().type);
        add_field(data, "first_event_subject", save_id_text(batch.events.front().subject));
        add_field(data, "first_event_message", batch.events.front().message);
    }
    if (!batch.reserved_ids.empty()) {
        add_field(data, "first_reserved_id", save_id_text(batch.reserved_ids.front()));
    }

    if (batch.command_sequence == 0) {
        add_issue(data, InspectionSeverity::warning, "replication_batch.zero_sequence",
                  "replication batch has command sequence 0");
    }
    if (batch.command_type.empty()) {
        add_issue(data, InspectionSeverity::warning, "replication_batch.missing_command_type",
                  "replication batch has no source command type");
    }
    if (batch.events.empty()) {
        add_issue(data, InspectionSeverity::warning, "replication_batch.no_events",
                  "replication batch carries no world-operation events");
    }
    return data;
}

InspectionData Inspector::inspect(const net::ReplicationRelevanceReport& report) {
    InspectionData data;
    data.object_type = "replication_relevance_report";
    data.display_name = "Replication Relevance Report";
    data.runtime_id = std::to_string(net::replication_stream_sequence(report));
    if (report.candidate_client_count == 0) {
        data.state = "no_clients";
    } else if (report.relevant_client_count == 0) {
        data.state = "filtered";
    } else if (report.filtered_client_count == 0) {
        data.state = "broadcast";
    } else {
        data.state = "partial";
    }

    add_field(data, "command_sequence", std::to_string(report.command_sequence));
    add_field(data, "replication_sequence",
              std::to_string(net::replication_stream_sequence(report)));
    add_field(data, "source_client_id", net_id_text(report.source_client_id));
    add_field(data, "command_type", report.command_type);
    add_field(data, "broadcast_by_default", bool_text(report.broadcast_by_default));
    add_field(data, "event_count", std::to_string(report.event_count));
    add_field(data, "candidate_client_count", std::to_string(report.candidate_client_count));
    add_field(data, "relevant_client_count", std::to_string(report.relevant_client_count));
    add_field(data, "filtered_client_count", std::to_string(report.filtered_client_count));
    add_field(data, "decision_count", std::to_string(report.decisions.size()));

    for (const auto& decision : report.decisions) {
        if (decision.relevant) {
            add_field(data, "first_relevant_client_id", net_id_text(decision.client_id));
            add_field(data, "first_relevant_reason", decision.reason);
            add_field(data, "first_relevant_event_count",
                      std::to_string(decision.relevant_event_count));
            break;
        }
    }
    for (const auto& decision : report.decisions) {
        if (!decision.relevant) {
            add_field(data, "first_filtered_client_id", net_id_text(decision.client_id));
            add_field(data, "first_filtered_reason", decision.reason);
            break;
        }
    }

    if (report.event_count > 0 && report.candidate_client_count > 0 &&
        report.relevant_client_count == 0) {
        add_issue(data, InspectionSeverity::warning, "replication_relevance.no_recipients",
                  "replication events were filtered from every candidate client");
    }
    if (report.relevant_client_count + report.filtered_client_count !=
        report.candidate_client_count) {
        add_issue(data, InspectionSeverity::error, "replication_relevance.count_mismatch",
                  "relevance decision counts do not match candidate client count");
    }
    if (report.decisions.size() != report.candidate_client_count) {
        add_issue(data, InspectionSeverity::error, "replication_relevance.decision_mismatch",
                  "relevance decision list does not match candidate client count");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const net::ReplicationIntakeReport& report) {
    InspectionData data;
    data.object_type = "replication_intake_report";
    data.display_name = "Replication Intake Report";
    data.runtime_id = std::to_string(report.last_sequence);
    if (report.batch_count == 0) {
        data.state = "empty";
    } else if (report.has_subject_events && report.has_global_events) {
        data.state = "mixed_events";
    } else if (report.has_subject_events) {
        data.state = "subject_events";
    } else if (report.has_global_events) {
        data.state = "global_events";
    } else {
        data.state = "metadata";
    }

    std::uint32_t batch_event_count = 0;
    std::uint32_t batch_reserved_id_count = 0;
    for (const auto& batch : report.batches) {
        batch_event_count += batch.event_count;
        batch_reserved_id_count += batch.reserved_id_count;
    }

    add_field(data, "batch_count", std::to_string(report.batch_count));
    add_field(data, "event_count", std::to_string(report.event_count));
    add_field(data, "reserved_id_count", std::to_string(report.reserved_id_count));
    add_field(data, "strictly_increasing_sequences",
              bool_text(report.strictly_increasing_sequences));
    add_field(data, "has_global_events", bool_text(report.has_global_events));
    add_field(data, "has_subject_events", bool_text(report.has_subject_events));
    add_field(data, "first_sequence", std::to_string(report.first_sequence));
    add_field(data, "last_sequence", std::to_string(report.last_sequence));
    add_field(data, "batch_report_count", std::to_string(report.batches.size()));
    if (!report.batches.empty()) {
        const auto& first = report.batches.front();
        add_field(data, "first_batch_sequence", std::to_string(first.command_sequence));
        add_field(data, "first_batch_command_type", first.command_type);
        add_field(data, "first_batch_event_count", std::to_string(first.event_count));
        add_field(data, "first_batch_reserved_id_count", std::to_string(first.reserved_id_count));
    }

    if (report.batches.size() != report.batch_count) {
        add_issue(data, InspectionSeverity::error, "replication_intake.batch_mismatch",
                  "replication intake batch count does not match batch reports");
    }
    if (batch_event_count != report.event_count ||
        batch_reserved_id_count != report.reserved_id_count) {
        add_issue(data, InspectionSeverity::error, "replication_intake.count_mismatch",
                  "replication intake event or reserved-id counts do not add up");
    }
    if (!report.strictly_increasing_sequences) {
        add_issue(data, InspectionSeverity::error, "replication_intake.sequence_order",
                  "replication intake batch sequences are not strictly increasing");
    }
    if (report.batch_count > 0 && report.event_count == 0) {
        add_issue(data, InspectionSeverity::warning, "replication_intake.no_events",
                  "replication intake has batches but no world-operation events");
    }
    if (report.batch_count > 0 && report.first_sequence == 0) {
        add_issue(data, InspectionSeverity::warning, "replication_intake.zero_first_sequence",
                  "replication intake starts at command sequence 0");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const net::HostSessionTickResult& result) {
    InspectionData data;
    data.object_type = "host_session_tick_result";
    data.display_name = "Host Session Tick Result";

    const auto relevant_client_count =
        relevance_relevant_client_total(result.replication_relevance_reports);
    const auto filtered_client_count =
        relevance_filtered_client_total(result.replication_relevance_reports);

    if (result.transport_dropped_reliable_message_count > 0) {
        data.state = "dropped_reliable";
    } else if (result.outbound_delivery.overload_disconnected_client_count > 0) {
        data.state = "reliable_overload";
    } else if (result.outbound_delivery.pending_message_count > 0) {
        data.state = "delivery_deferred";
    } else if (result.replication_message_count > 0) {
        data.state = "replicated";
    } else if (!result.command_reports.empty()) {
        data.state = "commands";
    } else if (result.transport_retransmission_count > 0) {
        data.state = "maintenance";
    } else if (result.outbound_delivery.delivered_message_count > 0) {
        data.state = "outbound_delivery";
    } else {
        data.state = "idle";
    }

    add_field(data, "transport_retransmission_count",
              std::to_string(result.transport_retransmission_count));
    add_field(data, "transport_dropped_reliable_message_count",
              std::to_string(result.transport_dropped_reliable_message_count));
    add_field(data, "transport_malformed_datagram_count",
              std::to_string(result.transport_malformed_datagram_count));
    add_field(data, "transport_rejected_datagram_count",
              std::to_string(result.transport_rejected_datagram_count));
    add_field(data, "transport_rate_limited_datagram_count",
              std::to_string(result.transport_rate_limited_datagram_count));
    add_field(data, "transport_client_to_server_bytes",
              std::to_string(result.transport_client_to_server_bytes));
    add_field(data, "transport_server_to_client_bytes",
              std::to_string(result.transport_server_to_client_bytes));
    add_field(data, "transport_client_to_server_message_count",
              std::to_string(result.transport_client_to_server_message_count));
    add_field(data, "transport_server_to_client_message_count",
              std::to_string(result.transport_server_to_client_message_count));
    add_field(data, "transport_simulated_dropped_unreliable_message_count",
              std::to_string(result.transport_simulated_dropped_unreliable_message_count));
    add_field(data, "transport_pending_impaired_message_count",
              std::to_string(result.transport_pending_impaired_message_count));
    add_field(data, "outbound_budget_dropped_unreliable_message_count",
              std::to_string(result.outbound_budget_dropped_unreliable_message_count));
    add_field(data, "discarded_disconnected_message_count",
              std::to_string(result.discarded_disconnected_message_count));
    add_field(data, "transport_message_count", std::to_string(result.transport_message_count));
    add_field(data, "command_message_count", std::to_string(result.command_message_count));
    add_field(data, "control_message_count", std::to_string(result.control_message_count));
    add_field(data, "response_message_count", std::to_string(result.response_message_count));
    add_field(data, "replication_message_count", std::to_string(result.replication_message_count));
    add_field(data, "outbound_delivery_initial_pending_message_count",
              std::to_string(result.outbound_delivery.initial_pending_message_count));
    add_field(data, "outbound_delivery_initial_pending_bytes",
              std::to_string(result.outbound_delivery.initial_pending_bytes));
    add_field(data, "outbound_delivery_attempted_message_count",
              std::to_string(result.outbound_delivery.attempted_message_count));
    add_field(data, "outbound_delivery_attempted_bytes",
              std::to_string(result.outbound_delivery.attempted_bytes));
    add_field(data, "outbound_delivery_delivered_message_count",
              std::to_string(result.outbound_delivery.delivered_message_count));
    add_field(data, "outbound_delivery_delivered_bytes",
              std::to_string(result.outbound_delivery.delivered_bytes));
    add_field(data, "outbound_delivery_retry_attempt_count",
              std::to_string(result.outbound_delivery.retry_attempt_count));
    add_field(data, "outbound_delivery_failed_attempt_count",
              std::to_string(result.outbound_delivery.failed_attempt_count));
    add_field(data, "outbound_delivery_pending_message_count",
              std::to_string(result.outbound_delivery.pending_message_count));
    add_field(data, "outbound_delivery_pending_bytes",
              std::to_string(result.outbound_delivery.pending_bytes));
    add_field(data, "outbound_delivery_blocked_client_count",
              std::to_string(result.outbound_delivery.blocked_client_count));
    add_field(data, "outbound_delivery_budget_deferred_message_count",
              std::to_string(result.outbound_delivery.budget_deferred_message_count));
    add_field(data, "outbound_delivery_tick_budget_deferred_message_count",
              std::to_string(result.outbound_delivery.tick_budget_deferred_message_count));
    add_field(data, "outbound_delivery_overload_disconnected_client_count",
              std::to_string(result.outbound_delivery.overload_disconnected_client_count));
    add_field(data, "command_report_count", std::to_string(result.command_reports.size()));
    add_field(data, "replication_relevance_report_count",
              std::to_string(result.replication_relevance_reports.size()));
    add_field(data, "replication_relevant_client_count", std::to_string(relevant_client_count));
    add_field(data, "replication_filtered_client_count", std::to_string(filtered_client_count));

    std::size_t accepted_command_count = 0;
    std::size_t rejected_command_count = 0;
    std::size_t committed_command_count = 0;
    for (const auto& report : result.command_reports) {
        if (report.success) {
            ++accepted_command_count;
        } else {
            ++rejected_command_count;
        }
        if (report.committed_world_mutation) {
            ++committed_command_count;
        }
    }
    add_field(data, "accepted_command_count", std::to_string(accepted_command_count));
    add_field(data, "rejected_command_count", std::to_string(rejected_command_count));
    add_field(data, "committed_command_count", std::to_string(committed_command_count));
    if (!result.command_reports.empty()) {
        add_field(data, "first_command_type", result.command_reports.front().command_type);
        add_field(data, "first_client_id", net_id_text(result.command_reports.front().client_id));
    }
    if (!result.replication_relevance_reports.empty()) {
        add_field(data, "first_relevance_command_type",
                  result.replication_relevance_reports.front().command_type);
    }
    if (!result.outbound_delivery.failures.empty()) {
        const auto& failure = result.outbound_delivery.failures.front();
        add_field(data, "first_delivery_failure_client_id", net_id_text(failure.client_id));
        add_field(data, "first_delivery_failure_message_kind",
                  std::string(net::transport_message_kind_name(failure.message_kind)));
        add_field(data, "first_delivery_failure_sequence", std::to_string(failure.sequence));
        add_field(data, "first_delivery_failure_attempt_count",
                  std::to_string(failure.attempt_count));
        add_field(data, "first_delivery_failure_error_code", failure.error_code);
    }

    if (result.response_message_count != result.command_reports.size()) {
        add_issue(data, InspectionSeverity::error, "host_tick.response_count_mismatch",
                  "host session tick response count must match command report count");
    }
    if (result.transport_message_count != result.command_reports.size() +
                                              result.control_messages.size() +
                                              result.discarded_disconnected_message_count) {
        add_issue(data, InspectionSeverity::error, "host_tick.transport_count_mismatch",
                  "host session tick transport message count must match command, control, and "
                  "discarded-disconnected message counts");
    }
    if (result.replication_message_count != relevant_client_count) {
        add_issue(data, InspectionSeverity::error, "host_tick.replication_count_mismatch",
                  "host session tick replication message count must match relevance recipients");
    }
    const auto delivery_status =
        net::validate_host_session_outbound_delivery_report(result.outbound_delivery);
    if (!delivery_status) {
        add_issue(data, InspectionSeverity::error, delivery_status.error().code,
                  delivery_status.error().message);
    }
    for (const auto client_id : result.outbound_delivery.overload_disconnected_clients) {
        if (std::ranges::find(result.disconnected_clients, client_id) ==
            result.disconnected_clients.end()) {
            add_issue(data, InspectionSeverity::error, "host_tick.overload_disconnect_missing",
                      "reliable overload client must also appear in disconnected clients");
            break;
        }
    }
    if (result.outbound_delivery.pending_message_count > 0) {
        add_issue(data, InspectionSeverity::warning, "host_tick.outbound_delivery_deferred",
                  "outbound messages remain queued for retry after a delivery failure");
    }
    if (result.transport_dropped_reliable_message_count > 0) {
        add_issue(data, InspectionSeverity::warning, "host_tick.reliable_messages_dropped",
                  "host session tick dropped reliable transport messages");
    }
    if (result.outbound_delivery.overload_disconnected_client_count > 0) {
        add_issue(data, InspectionSeverity::warning,
                  "host_tick.reliable_backlog_overload_disconnect",
                  "one or more clients were disconnected to preserve reliable backlog bounds");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const replay::CommandReplayLog& log) {
    InspectionData data;
    data.object_type = "command_replay_log";
    data.display_name = log.scenario_id.empty() ? "Command Replay Log" : log.scenario_id;
    data.runtime_id = std::to_string(log.world_seed);
    data.state = log.commands.empty() ? "empty" : "recorded";
    add_field(data, "version", std::to_string(log.version));
    add_field(data, "scenario_id", log.scenario_id);
    add_field(data, "world_seed", std::to_string(log.world_seed));
    add_field(data, "command_count", std::to_string(log.commands.size()));

    std::size_t expectation_count = 0;
    std::size_t mutating_expectation_count = 0;
    std::size_t event_expectation_count = 0;
    std::size_t reserved_id_expectation_count = 0;
    for (const auto& command : log.commands) {
        if (!command.expectation.empty()) {
            ++expectation_count;
        }
        if (command.expectation.has_committed_world_mutation &&
            command.expectation.committed_world_mutation) {
            ++mutating_expectation_count;
        }
        if (command.expectation.event_count.has_value() ||
            !command.expectation.event_types.empty()) {
            ++event_expectation_count;
        }
        if (command.expectation.reserved_id_count.has_value() ||
            !command.expectation.reserved_ids.empty()) {
            ++reserved_id_expectation_count;
        }
    }

    add_field(data, "expectation_count", std::to_string(expectation_count));
    add_field(data, "mutating_expectation_count", std::to_string(mutating_expectation_count));
    add_field(data, "event_expectation_count", std::to_string(event_expectation_count));
    add_field(data, "reserved_id_expectation_count", std::to_string(reserved_id_expectation_count));

    if (!log.commands.empty()) {
        add_field(data, "first_sequence", std::to_string(log.commands.front().envelope.sequence));
        add_field(data, "first_command_type", log.commands.front().envelope.type);
        add_field(data, "last_sequence", std::to_string(log.commands.back().envelope.sequence));
        add_field(data, "last_command_type", log.commands.back().envelope.type);
    }

    add_status_issue(data, log.validate());
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const replay::CommandReplayStep& step) {
    InspectionData data;
    data.object_type = "command_replay_step";
    data.display_name = "Command Replay Step";
    data.runtime_id = std::to_string(step.sequence);
    data.state =
        step.success ? (step.committed_world_mutation ? "committed" : "accepted") : "rejected";
    add_field(data, "index", std::to_string(step.index));
    add_field(data, "sequence", std::to_string(step.sequence));
    add_field(data, "command_type", step.command_type);
    add_field(data, "success", bool_text(step.success));
    add_field(data, "committed_world_mutation", bool_text(step.committed_world_mutation));
    add_field(data, "event_count", std::to_string(step.events.size()));
    add_field(data, "reserved_id_count", std::to_string(step.reserved_ids.size()));
    add_field(data, "error_code", step.error_code);
    add_field(data, "error_message", step.error_message);
    add_field(data, "expectation_checked", bool_text(step.expectation_checked));
    add_operation_trace_fields(data, step.operation_trace);
    add_operation_trace_issues(data, step.operation_trace);
    if (!step.success && step.error_code.empty()) {
        add_issue(data, InspectionSeverity::error, "replay_step.failure_without_error",
                  "failed replay step has no error code");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const replay::CommandReplayReport& report) {
    InspectionData data;
    data.object_type = "command_replay_report";
    data.display_name = "Command Replay Report";
    data.state = report.steps.empty() ? "empty" : "complete";
    add_field(data, "step_count", std::to_string(report.steps.size()));

    std::size_t committed_count = 0;
    std::size_t succeeded_count = 0;
    std::size_t rejected_count = 0;
    std::size_t expectation_checked_count = 0;
    std::size_t event_count = 0;
    std::size_t reserved_id_count = 0;
    for (const auto& step : report.steps) {
        if (step.success) {
            ++succeeded_count;
        } else {
            ++rejected_count;
        }
        if (step.committed_world_mutation) {
            ++committed_count;
        }
        if (step.expectation_checked) {
            ++expectation_checked_count;
        }
        event_count += step.events.size();
        reserved_id_count += step.reserved_ids.size();
    }

    add_field(data, "succeeded_step_count", std::to_string(succeeded_count));
    add_field(data, "rejected_step_count", std::to_string(rejected_count));
    add_field(data, "committed_step_count", std::to_string(committed_count));
    add_field(data, "expectation_checked_count", std::to_string(expectation_checked_count));
    add_field(data, "event_count", std::to_string(event_count));
    add_field(data, "reserved_id_count", std::to_string(reserved_id_count));
    if (!report.steps.empty()) {
        add_field(data, "first_command_type", report.steps.front().command_type);
        add_field(data, "last_command_type", report.steps.back().command_type);
        add_field(data, "last_sequence", std::to_string(report.steps.back().sequence));
    }
    return data;
}

InspectionData Inspector::inspect(const net::ClientSession& session) {
    const auto stats = session.stats();
    const auto intake = session.replication_intake_report();

    InspectionData data;
    data.object_type = "client_session";
    data.display_name = "Client Session";
    data.runtime_id = net_id_text(session.client_id());
    data.state = std::string(net::client_session_state_name(session.state()));
    add_field(data, "expected_client_id", net_id_text(session.expected_client_id()));
    add_field(data, "client_id", net_id_text(session.client_id()));
    add_field(data, "server_id", net_id_text(session.server_id()));
    add_field(data, "next_command_sequence", std::to_string(session.next_command_sequence()));
    add_field(data, "pending_command_count", std::to_string(stats.pending_command_count));
    add_field(data, "queued_result_count", std::to_string(stats.queued_result_count));
    add_field(data, "queued_replication_batch_count",
              std::to_string(stats.queued_replication_batch_count));
    add_field(data, "queued_replication_event_count",
              std::to_string(stats.queued_replication_event_count));
    add_field(data, "queued_replication_message_count",
              std::to_string(stats.queued_replication_message_count));
    add_field(data, "first_queued_replication_message_payload_type",
              stats.first_queued_replication_message_payload_type);
    add_field(data, "queued_replication_reserved_id_count",
              std::to_string(intake.reserved_id_count));
    add_field(data, "queued_replication_strictly_increasing",
              bool_text(intake.strictly_increasing_sequences));
    add_field(data, "queued_replication_has_global_events", bool_text(intake.has_global_events));
    add_field(data, "queued_replication_has_subject_events", bool_text(intake.has_subject_events));
    add_field(data, "has_replication_sequence", bool_text(stats.has_replication_sequence));
    add_field(data, "last_replication_sequence", std::to_string(stats.last_replication_sequence));
    add_field(data, "disconnect_reason_code", stats.disconnect_reason_code);
    add_field(data, "disconnect_reason_message", stats.disconnect_reason_message);
    add_field(data, "disconnected_at_ms", std::to_string(stats.disconnected_at_ms));
    if (!session.is_connected()) {
        add_issue(data, InspectionSeverity::warning, "client_session.disconnected",
                  "client session has not accepted a server welcome");
    }
    if (!intake.strictly_increasing_sequences) {
        add_issue(data, InspectionSeverity::error, "client_session.replication_sequence_order",
                  "queued replication batches are not strictly increasing");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const items::ItemStack& stack) {
    InspectionData data;
    data.object_type = "item_stack";
    data.display_name = "Item Stack";
    data.prototype_id = prototype_text(stack.prototype_id);
    data.state = stack.is_empty() ? "empty" : "occupied";
    add_field(data, "count", std::to_string(stack.count));
    add_field(data, "max_count", std::to_string(stack.max_count));
    add_field(data, "quality", std::to_string(stack.quality));
    add_field(data, "remaining_capacity", std::to_string(stack.remaining_capacity()));

    if (stack.is_empty()) {
        add_issue(data, InspectionSeverity::warning, "item_stack.empty",
                  "item stack has zero count");
    }
    if (!stack.prototype_id.is_valid()) {
        add_issue(data, InspectionSeverity::error, "item_stack.invalid_prototype",
                  "item stack prototype id is invalid");
    }
    return data;
}

InspectionData Inspector::inspect(const cargo::CargoRecord& cargo_record) {
    InspectionData data;
    data.object_type = "cargo";
    data.display_name = "Cargo";
    data.save_id = save_id_text(cargo_record.cargo_id);
    data.prototype_id = prototype_text(cargo_record.prototype_id);
    data.state = cargo_record.is_hazardous() ? "hazardous" : "stable";
    const auto approximate_position = cargo_record.position.approximate_global();
    add_field(data, "position",
              std::to_string(approximate_position.x) + "|" +
                  std::to_string(approximate_position.y) + "|" +
                  std::to_string(approximate_position.z));
    add_field(data, "position_anchor",
              std::to_string(cargo_record.position.anchor.x) + "|" +
                  std::to_string(cargo_record.position.anchor.y) + "|" +
                  std::to_string(cargo_record.position.anchor.z));
    add_field(data, "position_local",
              std::to_string(cargo_record.position.local_offset.x) + "|" +
                  std::to_string(cargo_record.position.local_offset.y) + "|" +
                  std::to_string(cargo_record.position.local_offset.z));
    add_field(data, "mass_grams", std::to_string(cargo_record.mass_grams));
    add_field(data, "volume_milliliters", std::to_string(cargo_record.volume_milliliters));
    add_field(data, "stability_per_mille", std::to_string(cargo_record.stability_per_mille));
    add_field(data, "transport_mode_bits",
              std::to_string(cargo_record.allowed_transport_modes.bits()));
    add_field(data, "hazard_count", std::to_string(cargo_record.hazard_tags.size()));
    add_status_issue(data, cargo_record.validate());
    return data;
}

InspectionData Inspector::inspect(const entities::EntityRecord& entity) {
    InspectionData data;
    data.object_type = "entity";
    data.display_name = "Entity";
    data.save_id = save_id_text(entity.save_id);
    data.runtime_id = entity.runtime_handle.is_valid() ? entity.runtime_handle.to_string() : "";
    data.prototype_id = prototype_text(entity.prototype_id);
    data.state = entity.sleeping ? "sleeping" : "active";
    add_field(data, "net_id", entity.net_id.is_valid() ? entity.net_id.to_string() : "");
    add_field(data, "kind", entity_kind_text(entity.kind));
    const auto approximate_position = entity.transform.position.approximate_global();
    add_field(data, "position",
              std::to_string(approximate_position.x) + "|" +
                  std::to_string(approximate_position.y) + "|" +
                  std::to_string(approximate_position.z));
    add_field(data, "position_anchor",
              std::to_string(entity.transform.position.anchor.x) + "|" +
                  std::to_string(entity.transform.position.anchor.y) + "|" +
                  std::to_string(entity.transform.position.anchor.z));
    add_field(data, "position_local",
              std::to_string(entity.transform.position.local_offset.x) + "|" +
                  std::to_string(entity.transform.position.local_offset.y) + "|" +
                  std::to_string(entity.transform.position.local_offset.z));
    add_field(data, "rotation_degrees",
              std::to_string(entity.transform.rotation_degrees.x) + "|" +
                  std::to_string(entity.transform.rotation_degrees.y) + "|" +
                  std::to_string(entity.transform.rotation_degrees.z));
    add_field(data, "scale",
              std::to_string(entity.transform.scale.x) + "|" +
                  std::to_string(entity.transform.scale.y) + "|" +
                  std::to_string(entity.transform.scale.z));
    add_field(data, "persistent", bool_text(entity.persistent));
    add_status_issue(data, entity.validate());
    return data;
}

InspectionData Inspector::inspect(const entities::PhysicalResourceRecord& resource) {
    InspectionData data;
    data.object_type = "physical_resource";
    data.display_name = "Physical Resource";
    data.save_id = save_id_text(resource.resource_id);
    data.prototype_id = prototype_text(resource.prototype_id);
    data.runtime_id =
        resource.physics_body_id.is_valid() ? resource.physics_body_id.to_string() : "";
    data.state = std::string(entities::physical_resource_state_name(resource.state));
    add_field(data, "kind", std::string(entities::physical_resource_kind_name(resource.kind)));
    add_field(data, "cargo_prototype_id", prototype_text(resource.cargo_prototype_id));
    add_field(data, "mass_grams", std::to_string(resource.mass_grams));
    add_field(data, "volume_milliliters", std::to_string(resource.volume_milliliters));
    add_field(data, "stability_per_mille", std::to_string(resource.stability_per_mille));
    add_field(data, "transport_mode_bits", std::to_string(resource.allowed_transport_modes.bits()));
    add_field(data, "hazard_count", std::to_string(resource.hazard_tags.size()));
    add_field(data, "segment_count", std::to_string(resource.segments.size()));
    add_status_issue(data, resource.validate());
    return data;
}

InspectionData Inspector::inspect(const build::BuildPieceRecord& build_piece) {
    InspectionData data;
    data.object_type = "build_piece";
    data.display_name = "Build Piece";
    data.save_id = save_id_text(build_piece.object_id);
    data.prototype_id = prototype_text(build_piece.prototype_id);
    data.state = construction_state_text(build_piece.construction_state);
    add_field(data, "socket_count", std::to_string(build_piece.sockets.size()));
    add_field(data, "network_port_count", std::to_string(build_piece.network_ports.size()));
    add_field(data, "material_tag_count", std::to_string(build_piece.material_tags.size()));
    add_field(data, "room_contribution_tag_count",
              std::to_string(build_piece.room_contribution_tags.size()));
    add_field(data, "contributes_to_rooms", bool_text(build_piece.contributes_to_rooms()));
    add_field(data, "exposes_network_ports", bool_text(build_piece.exposes_network_ports()));
    add_status_issue(data, build_piece.validate());
    return data;
}

InspectionData Inspector::inspect(const assemblies::AssemblyRecord& assembly) {
    InspectionData data;
    data.object_type = "assembly";
    data.display_name = "Assembly";
    data.save_id = save_id_text(assembly.assembly_id);
    data.prototype_id = prototype_text(assembly.prototype_id);
    data.state = assembly.operating ? "operating" : "idle";
    add_field(data, "root_build_piece_id", save_id_text(assembly.root_build_piece_id));
    add_field(data, "part_count", std::to_string(assembly.parts.size()));
    add_field(data, "port_count", std::to_string(assembly.ports.size()));
    add_field(data, "port_sources", assembly_port_sources(assembly.ports));
    add_field(data, "total_port_capacity",
              std::to_string(assembly_total_port_capacity(assembly.ports)));
    add_status_issue(data, assembly.validate_record());
    return data;
}

InspectionData Inspector::inspect(const processes::ProcessInstance& process) {
    InspectionData data;
    data.object_type = "process";
    data.display_name = "Process";
    data.save_id = save_id_text(process.owner_id);
    data.runtime_id = process.process_id.is_valid() ? process.process_id.to_string() : "";
    data.prototype_id = prototype_text(process.prototype_id);
    data.state = process_state_text(process.state);
    add_field(data, "start_time_ms", std::to_string(process.start_time_ms));
    add_field(data, "last_update_time_ms", std::to_string(process.last_update_time_ms));
    add_field(data, "required_effective_work_ms",
              std::to_string(process.required_effective_work_ms));
    add_field(data, "accumulated_effective_work_ms",
              std::to_string(process.accumulated_effective_work_ms));
    add_field(data, "remaining_effective_work_ms",
              std::to_string(process.remaining_effective_work_ms()));
    add_field(data, "input_slot_count", std::to_string(process.input_slots.size()));
    add_field(data, "output_slot_count", std::to_string(process.output_slots.size()));
    if (!process.interruption_reason.empty()) {
        add_issue(data, InspectionSeverity::warning, "process.interrupted",
                  process.interruption_reason);
    }
    if (!process.process_id.is_valid()) {
        add_issue(data, InspectionSeverity::error, "process.invalid_id", "process id is invalid");
    }
    if (!process.owner_id.is_valid()) {
        add_issue(data, InspectionSeverity::error, "process.invalid_owner",
                  "process owner save id is invalid");
    }
    return data;
}

InspectionData Inspector::inspect(const networks::SpatialNetwork& network) {
    InspectionData data;
    data.object_type = "spatial_network";
    data.display_name = "Spatial Network";
    data.state = network.is_dirty() ? "dirty" : "clean";
    add_field(data, "kind", std::string(networks::network_kind_name(network.kind())));
    add_field(data, "node_count", std::to_string(network.node_count()));
    add_field(data, "edge_count", std::to_string(network.edge_count()));
    add_field(data, "blocked_edge_count", std::to_string(network.blocked_edge_count()));
    add_field(data, "port_count", std::to_string(network.port_count()));
    add_field(data, "owned_port_count", std::to_string(network.owned_port_count()));
    add_field(data, "sourced_port_count", std::to_string(network.sourced_port_count()));
    add_field(data, "total_node_capacity", std::to_string(network.total_node_capacity()));
    add_field(data, "total_edge_capacity", std::to_string(network.total_edge_capacity()));
    add_field(data, "total_port_capacity", std::to_string(network.total_port_capacity()));
    add_field(data, "dirty_region_kind",
              std::string(
                  dirty::dirty_region_kind_name(networks::dirty_region_kind_for(network.kind()))));

    if (network.node_count() == 0) {
        add_issue(data, InspectionSeverity::warning, "network.empty",
                  "spatial network has no nodes");
    }
    if (network.blocked_edge_count() > 0) {
        add_issue(data, InspectionSeverity::warning, "network.blocked_edges",
                  "spatial network has blocked edges");
    }
    return data;
}

InspectionData Inspector::inspect(const world::RegionGraph& graph) {
    InspectionData data;
    data.object_type = "region_graph";
    data.display_name = "Region Graph";
    data.state = graph.region_count() == 0 ? "empty" : "defined";

    const auto regions = graph.regions();
    std::size_t sub_biome_count = 0;
    std::size_t resource_rule_count = 0;
    std::size_t future_tool_layer_count = 0;
    std::size_t mastery_return_layer_count = 0;
    std::size_t ecology_parameter_count = 0;
    double max_danger_gradient = 0.0;
    double max_magic_gradient = 0.0;

    for (const auto* region : regions) {
        sub_biome_count += region->sub_biomes.size();
        resource_rule_count += region->resource_rules.size();
        future_tool_layer_count += region->future_tool_layers.size();
        mastery_return_layer_count += region->mastery_return_layers.size();
        ecology_parameter_count += region->ecology_parameters.size();
        max_danger_gradient = std::max(max_danger_gradient, region->danger_gradient);
        max_magic_gradient = std::max(max_magic_gradient, region->magic_gradient);
    }

    add_field(data, "region_count", std::to_string(graph.region_count()));
    add_field(data, "connection_count", std::to_string(graph.connection_count()));
    add_field(data, "sub_biome_count", std::to_string(sub_biome_count));
    add_field(data, "resource_rule_count", std::to_string(resource_rule_count));
    add_field(data, "future_tool_layer_count", std::to_string(future_tool_layer_count));
    add_field(data, "mastery_return_layer_count", std::to_string(mastery_return_layer_count));
    add_field(data, "ecology_parameter_count", std::to_string(ecology_parameter_count));
    add_field(data, "max_danger_gradient", std::to_string(max_danger_gradient));
    add_field(data, "max_magic_gradient", std::to_string(max_magic_gradient));

    if (!regions.empty()) {
        const auto& first_region = *regions.front();
        add_field(data, "first_region_id", first_region.id);
        add_field(data, "first_region_age", first_region.age);
        add_field(data, "first_region_biome_cluster", first_region.biome_cluster);
        add_field(data, "first_region_connection_count",
                  std::to_string(graph.connections_for(first_region.id).size()));
    }

    if (graph.region_count() == 0) {
        add_issue(data, InspectionSeverity::warning, "region_graph.empty",
                  "region graph has no regions");
    }
    if (graph.region_count() > 1 && graph.connection_count() == 0) {
        add_issue(data, InspectionSeverity::warning, "region_graph.disconnected",
                  "region graph has multiple regions but no connections");
    }

    return data;
}

InspectionData Inspector::inspect(const dirty::DirtyRegionTracker& tracker) {
    InspectionData data;
    data.object_type = "dirty_region_tracker";
    data.display_name = "Dirty Region Tracker";
    data.state = tracker.empty() ? "clean" : "dirty";
    add_field(data, "region_count", std::to_string(tracker.size()));
    add_dirty_region_count_fields(data, tracker);

    if (!tracker.empty()) {
        std::uint64_t first_sequence = tracker.regions().front().sequence;
        std::uint64_t last_sequence = tracker.regions().front().sequence;
        for (const auto& region : tracker.regions()) {
            first_sequence = std::min(first_sequence, region.sequence);
            last_sequence = std::max(last_sequence, region.sequence);
        }

        const auto& first_region = tracker.regions().front();
        add_field(data, "first_sequence", std::to_string(first_sequence));
        add_field(data, "last_sequence", std::to_string(last_sequence));
        add_field(data, "first_region_kind",
                  std::string(dirty::dirty_region_kind_name(first_region.kind)));
        add_field(data, "first_region_bounds", dirty_bounds_text(first_region.bounds));
        add_field(data, "first_region_reason", first_region.reason);
    }

    return data;
}

InspectionData Inspector::inspect(const rooms::RoomGraph& graph) {
    InspectionData data;
    data.object_type = "room_graph";
    data.display_name = "Room Graph";

    auto rooms = graph.rooms();
    std::ranges::sort(rooms, [](const rooms::RoomRecord* lhs, const rooms::RoomRecord* rhs) {
        return lhs->id.value() < rhs->id.value();
    });

    std::uint64_t total_volume_cells = 0;
    std::size_t source_build_piece_ref_count = 0;
    std::size_t descriptor_count = 0;
    std::size_t positive_descriptor_count = 0;
    std::size_t neutral_descriptor_count = 0;
    std::size_t warning_descriptor_count = 0;

    for (const auto* room : rooms) {
        total_volume_cells += room->volume_cells;
        source_build_piece_ref_count += room->source_build_piece_ids.size();
        descriptor_count += room->descriptors.size();
        add_status_issue(data, room->validate());

        for (const auto& descriptor : room->descriptors) {
            switch (descriptor.severity) {
            case rooms::RoomDescriptorSeverity::positive:
                ++positive_descriptor_count;
                break;
            case rooms::RoomDescriptorSeverity::neutral:
                ++neutral_descriptor_count;
                break;
            case rooms::RoomDescriptorSeverity::warning:
                ++warning_descriptor_count;
                break;
            }
        }
    }

    add_field(data, "room_count", std::to_string(graph.room_count()));
    add_field(data, "total_volume_cells", std::to_string(total_volume_cells));
    add_field(data, "source_build_piece_ref_count", std::to_string(source_build_piece_ref_count));
    add_field(data, "descriptor_count", std::to_string(descriptor_count));
    add_field(data, "positive_descriptor_count", std::to_string(positive_descriptor_count));
    add_field(data, "neutral_descriptor_count", std::to_string(neutral_descriptor_count));
    add_field(data, "warning_descriptor_count", std::to_string(warning_descriptor_count));
    add_field(data, "exposed_room_count", std::to_string(graph.count_descriptor("exposed")));
    add_field(data, "smoky_room_count", std::to_string(graph.count_descriptor("smoky")));
    add_field(data, "poor_cart_access_room_count",
              std::to_string(graph.count_descriptor("poor_cart_access")));
    add_field(data, "storage_access_room_count",
              std::to_string(graph.count_descriptor("storage_access")));
    add_field(data, "cart_access_room_count",
              std::to_string(graph.count_descriptor("cart_access")));
    add_field(data, "power_access_room_count",
              std::to_string(graph.count_descriptor("power_access")));
    add_field(data, "warded_room_count", std::to_string(graph.count_descriptor("warded")));

    if (!rooms.empty()) {
        const auto& first_room = *rooms.front();
        add_field(data, "first_room_id", first_room.id.to_string());
        add_field(data, "first_room_label", first_room.label);
        add_field(data, "first_room_volume_cells", std::to_string(first_room.volume_cells));
        add_field(data, "first_room_descriptor_count",
                  std::to_string(first_room.descriptors.size()));
    }

    if (rooms.empty()) {
        add_issue(data, InspectionSeverity::warning, "room_graph.empty", "room graph has no rooms");
    }
    if (warning_descriptor_count > 0) {
        add_issue(data, InspectionSeverity::warning, "room_graph.warning_descriptors",
                  "room graph contains rooms with warning descriptors");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    } else if (graph.is_dirty()) {
        data.state = "dirty";
    } else if (rooms.empty()) {
        data.state = "empty";
    } else {
        data.state = "derived";
    }

    return data;
}

InspectionData Inspector::inspect(const rooms::RoomRecord& room) {
    InspectionData data;
    data.object_type = "room";
    data.display_name = room.label.empty() ? "Room" : room.label;
    data.runtime_id = room.id.is_valid() ? room.id.to_string() : "";
    data.state = room.has_descriptor("exposed") ? "exposed" : "derived";
    add_field(data, "volume_cells", std::to_string(room.volume_cells));
    add_field(data, "source_build_piece_count", std::to_string(room.source_build_piece_ids.size()));
    add_field(data, "descriptor_count", std::to_string(room.descriptors.size()));
    add_field(data, "descriptor_severity_counts", descriptor_severity_counts(room.descriptors));
    add_field(data, "enclosure_per_mille", std::to_string(room.metrics.enclosure_per_mille));
    add_field(data, "roof_coverage_per_mille",
              std::to_string(room.metrics.roof_coverage_per_mille));
    add_field(data, "smoke_per_mille", std::to_string(room.metrics.smoke_per_mille));
    add_field(data, "storage_access", bool_text(room.metrics.storage_access));
    add_field(data, "cart_access", bool_text(room.metrics.cart_access));
    add_status_issue(data, room.validate());

    for (const auto& descriptor : room.descriptors) {
        if (descriptor.severity == rooms::RoomDescriptorSeverity::warning) {
            add_issue(data, InspectionSeverity::warning, "room.descriptor." + descriptor.code,
                      descriptor.label);
        }
    }

    return data;
}

InspectionData Inspector::inspect(const workpieces::WorkpieceGrid& grid) {
    InspectionData data;
    const auto shape = grid.shape();
    const auto total_cells = static_cast<std::size_t>(shape.width) *
                             static_cast<std::size_t>(shape.height) *
                             static_cast<std::size_t>(shape.depth);

    data.object_type = "workpiece_grid";
    data.display_name = "Workpiece Grid";
    data.state = grid.occupied_count() == 0 ? "empty" : "occupied";
    add_field(data, "width", std::to_string(shape.width));
    add_field(data, "height", std::to_string(shape.height));
    add_field(data, "depth", std::to_string(shape.depth));
    add_field(data, "total_cells", std::to_string(total_cells));
    add_field(data, "occupied_cells", std::to_string(grid.occupied_count()));
    add_field(data, "operation_count", std::to_string(grid.history().size()));
    add_field(data, "material_counts", workpiece_material_counts(grid));
    return data;
}

InspectionData Inspector::inspect(const world::WorldReplicationDeltaPlan& plan) {
    InspectionData data;
    data.object_type = "world_replication_delta_plan";
    data.display_name = "World Replication Delta Plan";
    data.runtime_id = std::to_string(plan.replication_sequence != 0 ? plan.replication_sequence
                                                                    : plan.command_sequence);
    if (plan.event_count == 0) {
        data.state = "empty";
    } else if (plan.requires_snapshot_resync) {
        data.state = "needs_resync";
    } else if (plan.has_global_events && !plan.subjects.empty()) {
        data.state = "mixed";
    } else if (plan.has_global_events) {
        data.state = "global_events";
    } else {
        data.state = "subjects";
    }

    std::uint32_t subject_event_count = 0;
    std::uint32_t materialized_record_count = 0;
    std::uint32_t missing_subject_count = 0;
    for (const auto& subject : plan.subjects) {
        subject_event_count += subject.event_count;
        materialized_record_count += subject.materialized_record_count;
        missing_subject_count += subject.missing_subject ? 1U : 0U;
    }

    add_field(data, "command_sequence", std::to_string(plan.command_sequence));
    add_field(data, "replication_sequence",
              std::to_string(plan.replication_sequence != 0 ? plan.replication_sequence
                                                            : plan.command_sequence));
    add_field(data, "source_client_id", net_id_text(plan.source_client_id));
    add_field(data, "command_type", plan.command_type);
    add_field(data, "event_count", std::to_string(plan.event_count));
    add_field(data, "global_event_count", std::to_string(plan.global_event_count));
    add_field(data, "subject_event_count", std::to_string(plan.subject_event_count));
    add_field(data, "unique_subject_count", std::to_string(plan.unique_subject_count));
    add_field(data, "missing_subject_count", std::to_string(plan.missing_subject_count));
    add_field(data, "materialized_record_count", std::to_string(plan.materialized_record_count));
    add_field(data, "has_global_events", bool_text(plan.has_global_events));
    add_field(data, "requires_snapshot_resync", bool_text(plan.requires_snapshot_resync));
    add_field(data, "global_event_report_count", std::to_string(plan.global_events.size()));
    add_field(data, "subject_report_count", std::to_string(plan.subjects.size()));
    if (!plan.global_events.empty()) {
        add_field(data, "first_global_event_type", plan.global_events.front().type);
        add_field(data, "first_global_event_message", plan.global_events.front().message);
    }
    if (!plan.subjects.empty()) {
        const auto& first = plan.subjects.front();
        add_field(data, "first_subject_id", save_id_text(first.subject_id));
        add_field(data, "first_subject_event_count", std::to_string(first.event_count));
        add_field(data, "first_subject_first_event_type", first.first_event_type);
        add_field(data, "first_subject_materialized_record_count",
                  std::to_string(first.materialized_record_count));
        add_field(data, "first_subject_has_build_piece", bool_text(first.has_build_piece));
        add_field(data, "first_subject_has_entity", bool_text(first.has_entity));
        add_field(data, "first_subject_has_cargo", bool_text(first.has_cargo));
        add_field(data, "first_subject_has_assembly", bool_text(first.has_assembly));
        add_field(data, "first_subject_has_inventory", bool_text(first.has_inventory));
        add_field(data, "first_subject_process_count", std::to_string(first.process_count));
    }

    if (plan.global_events.size() != plan.global_event_count) {
        add_issue(data, InspectionSeverity::error, "world_replication_delta.global_count_mismatch",
                  "world replication delta global event count does not match global events");
    }
    if (plan.subjects.size() != plan.unique_subject_count) {
        add_issue(data, InspectionSeverity::error, "world_replication_delta.subject_count_mismatch",
                  "world replication delta subject count does not match subject reports");
    }
    if (plan.subject_event_count + plan.global_event_count != plan.event_count ||
        subject_event_count != plan.subject_event_count ||
        materialized_record_count != plan.materialized_record_count ||
        missing_subject_count != plan.missing_subject_count) {
        add_issue(data, InspectionSeverity::error, "world_replication_delta.count_mismatch",
                  "world replication delta aggregate counts do not add up");
    }
    if (plan.missing_subject_count > 0) {
        add_issue(data, InspectionSeverity::warning, "world_replication_delta.missing_subjects",
                  "world replication delta references save ids that are not present in the world");
    }
    if (plan.event_count > 0 && plan.subject_event_count == 0 && plan.global_event_count == 0) {
        add_issue(data, InspectionSeverity::error, "world_replication_delta.no_classified_events",
                  "world replication delta has events that are neither global nor subject events");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const world::WorldReplicationDeltaSnapshot& snapshot) {
    InspectionData data;
    data.object_type = "world_replication_delta_snapshot";
    data.display_name = "World Replication Delta Snapshot";
    data.runtime_id =
        std::to_string(snapshot.plan.replication_sequence != 0 ? snapshot.plan.replication_sequence
                                                               : snapshot.plan.command_sequence);

    const auto section_record_count = static_cast<std::uint32_t>(
        snapshot.build_pieces.size() + snapshot.entities.size() + snapshot.cargo_records.size() +
        snapshot.inventories.size() + snapshot.assemblies.size() + snapshot.processes.size());

    if (snapshot.plan.event_count == 0 && section_record_count == 0) {
        data.state = "empty";
    } else if (snapshot.plan.requires_snapshot_resync) {
        data.state = "needs_resync";
    } else {
        data.state = "materialized";
    }

    add_field(data, "command_sequence", std::to_string(snapshot.plan.command_sequence));
    add_field(data, "replication_sequence",
              std::to_string(snapshot.plan.replication_sequence != 0
                                 ? snapshot.plan.replication_sequence
                                 : snapshot.plan.command_sequence));
    add_field(data, "source_client_id", net_id_text(snapshot.plan.source_client_id));
    add_field(data, "command_type", snapshot.plan.command_type);
    add_field(data, "event_count", std::to_string(snapshot.plan.event_count));
    add_field(data, "global_event_count", std::to_string(snapshot.plan.global_event_count));
    add_field(data, "unique_subject_count", std::to_string(snapshot.plan.unique_subject_count));
    add_field(data, "missing_subject_count", std::to_string(snapshot.plan.missing_subject_count));
    add_field(data, "planned_materialized_record_count",
              std::to_string(snapshot.plan.materialized_record_count));
    add_field(data, "section_record_count", std::to_string(section_record_count));
    add_field(data, "build_piece_count", std::to_string(snapshot.build_pieces.size()));
    add_field(data, "entity_count", std::to_string(snapshot.entities.size()));
    add_field(data, "cargo_count", std::to_string(snapshot.cargo_records.size()));
    add_field(data, "inventory_count", std::to_string(snapshot.inventories.size()));
    add_field(data, "assembly_count", std::to_string(snapshot.assemblies.size()));
    add_field(data, "process_count", std::to_string(snapshot.processes.size()));
    add_field(data, "requires_snapshot_resync", bool_text(snapshot.plan.requires_snapshot_resync));
    if (!snapshot.build_pieces.empty()) {
        add_field(data, "first_build_piece_id",
                  save_id_text(snapshot.build_pieces.front().object_id));
    }
    if (!snapshot.entities.empty()) {
        add_field(data, "first_entity_id", save_id_text(snapshot.entities.front().save_id));
    }
    if (!snapshot.cargo_records.empty()) {
        add_field(data, "first_cargo_id", save_id_text(snapshot.cargo_records.front().cargo_id));
    }
    if (!snapshot.inventories.empty()) {
        add_field(data, "first_inventory_owner_id",
                  save_id_text(snapshot.inventories.front().owner_id));
    }
    if (!snapshot.assemblies.empty()) {
        add_field(data, "first_assembly_id", save_id_text(snapshot.assemblies.front().assembly_id));
    }
    if (!snapshot.processes.empty()) {
        add_field(data, "first_process_id", process_id_text(snapshot.processes.front().process_id));
    }

    auto plan_inspection = inspect(snapshot.plan);
    for (const auto& issue : plan_inspection.issues) {
        add_issue(data, issue.severity, issue.code, issue.message);
    }
    if (section_record_count != snapshot.plan.materialized_record_count) {
        add_issue(data, InspectionSeverity::error,
                  "world_replication_delta_snapshot.record_count_mismatch",
                  "world replication delta snapshot section counts do not match the plan");
    }
    if (snapshot.plan.requires_snapshot_resync) {
        add_issue(data, InspectionSeverity::warning,
                  "world_replication_delta_snapshot.requires_resync",
                  "world replication delta snapshot is partial and needs snapshot/resync fallback");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const world::WorldReplicationDeltaTickReport& report) {
    InspectionData data;
    data.object_type = "world_replication_delta_tick_report";
    data.display_name = "World Replication Delta Tick Report";
    if (report.command_report_count == 0) {
        data.state = "empty";
    } else if (report.requires_snapshot_resync) {
        data.state = "needs_resync";
    } else if (report.materialized_command_count == 0) {
        data.state = "skipped";
    } else {
        data.state = "materialized";
    }

    std::uint32_t materialized_command_count = 0;
    std::uint32_t skipped_command_count = 0;
    std::uint32_t total_event_count = 0;
    std::uint32_t total_materialized_record_count = 0;
    for (const auto& command : report.commands) {
        if (command.skipped) {
            ++skipped_command_count;
        } else {
            ++materialized_command_count;
            total_event_count += command.snapshot.plan.event_count;
            total_materialized_record_count += command.snapshot.plan.materialized_record_count;
        }
    }

    add_field(data, "command_report_count", std::to_string(report.command_report_count));
    add_field(data, "materialized_command_count",
              std::to_string(report.materialized_command_count));
    add_field(data, "skipped_command_count", std::to_string(report.skipped_command_count));
    add_field(data, "total_event_count", std::to_string(report.total_event_count));
    add_field(data, "total_materialized_record_count",
              std::to_string(report.total_materialized_record_count));
    add_field(data, "requires_snapshot_resync", bool_text(report.requires_snapshot_resync));
    add_field(data, "command_delta_count", std::to_string(report.commands.size()));
    for (const auto& command : report.commands) {
        if (!command.skipped) {
            add_field(data, "first_materialized_sequence",
                      std::to_string(command.command_sequence));
            add_field(data, "first_materialized_replication_sequence",
                      std::to_string(command.replication_sequence));
            add_field(data, "first_materialized_command_type", command.command_type);
            add_field(data, "first_materialized_event_count",
                      std::to_string(command.snapshot.plan.event_count));
            break;
        }
    }
    for (const auto& command : report.commands) {
        if (command.skipped) {
            add_field(data, "first_skipped_sequence", std::to_string(command.command_sequence));
            add_field(data, "first_skipped_command_type", command.command_type);
            add_field(data, "first_skipped_reason", command.skip_reason);
            add_field(data, "first_skipped_error_code", command.error_code);
            add_field(data, "first_skipped_error_message", command.error_message);
            add_field(data, "first_skipped_stage_count",
                      std::to_string(command.operation_trace.stages.size()));
            add_field(data, "first_skipped_last_stage",
                      command.operation_trace.stages.empty()
                          ? ""
                          : std::string(world::operation_stage_name(
                                command.operation_trace.stages.back())));
            add_field(data, "first_skipped_mutation_count",
                      std::to_string(command.operation_trace.mutations.size()));
            add_field(data, "first_skipped_replication_dirty",
                      bool_text(command.operation_trace.replication_dirty));
            add_field(data, "first_skipped_save_dirty",
                      bool_text(command.operation_trace.save_dirty));
            break;
        }
    }

    if (report.commands.size() != report.command_report_count) {
        add_issue(data, InspectionSeverity::error,
                  "world_replication_delta_tick.command_count_mismatch",
                  "world replication delta tick command count does not match command reports");
    }
    if (materialized_command_count != report.materialized_command_count ||
        skipped_command_count != report.skipped_command_count ||
        total_event_count != report.total_event_count ||
        total_materialized_record_count != report.total_materialized_record_count) {
        add_issue(data, InspectionSeverity::error, "world_replication_delta_tick.count_mismatch",
                  "world replication delta tick aggregate counts do not add up");
    }
    if (report.command_report_count > 0 && report.materialized_command_count == 0) {
        add_issue(data, InspectionSeverity::warning,
                  "world_replication_delta_tick.no_materialized_commands",
                  "world replication delta tick did not materialize any command deltas");
    }
    if (report.requires_snapshot_resync) {
        add_issue(data, InspectionSeverity::warning, "world_replication_delta_tick.requires_resync",
                  "world replication delta tick contains at least one partial delta");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const world::WorldReplicationDeltaDeliveryReport& report) {
    InspectionData data;
    data.object_type = "world_replication_delta_delivery_report";
    data.display_name = "World Replication Delta Delivery Report";
    if (report.command_delta_count == 0) {
        data.state = "empty";
    } else if (report.sent_message_count > 0 && report.skipped_command_count > 0) {
        data.state = "partial";
    } else if (report.sent_message_count > 0) {
        data.state = "sent";
    } else {
        data.state = "skipped";
    }

    std::uint32_t materialized_command_count = 0;
    std::uint32_t skipped_command_count = 0;
    std::uint32_t sent_message_count = 0;
    for (const auto& command : report.commands) {
        if (command.skipped) {
            ++skipped_command_count;
        } else {
            ++materialized_command_count;
        }
        sent_message_count += command.sent_message_count;
    }

    add_field(data, "command_delta_count", std::to_string(report.command_delta_count));
    add_field(data, "materialized_command_count",
              std::to_string(report.materialized_command_count));
    add_field(data, "skipped_command_count", std::to_string(report.skipped_command_count));
    add_field(data, "relevance_report_count", std::to_string(report.relevance_report_count));
    add_field(data, "sent_message_count", std::to_string(report.sent_message_count));
    add_field(data, "unmatched_relevance_count", std::to_string(report.unmatched_relevance_count));
    add_field(data, "resync_skipped_count", std::to_string(report.resync_skipped_count));
    add_field(data, "command_report_count", std::to_string(report.commands.size()));
    for (const auto& command : report.commands) {
        if (!command.skipped) {
            add_field(data, "first_sent_sequence", std::to_string(command.command_sequence));
            add_field(data, "first_sent_replication_sequence",
                      std::to_string(command.replication_sequence));
            add_field(data, "first_sent_command_type", command.command_type);
            add_field(data, "first_sent_message_count", std::to_string(command.sent_message_count));
            if (!command.recipients.empty()) {
                add_field(data, "first_recipient_id", net_id_text(command.recipients.front()));
            }
            break;
        }
    }
    for (const auto& command : report.commands) {
        if (command.skipped) {
            add_field(data, "first_skipped_sequence", std::to_string(command.command_sequence));
            add_field(data, "first_skipped_command_type", command.command_type);
            add_field(data, "first_skipped_reason", command.skip_reason);
            add_field(data, "first_skipped_error_code", command.error_code);
            add_field(data, "first_skipped_error_message", command.error_message);
            add_field(data, "first_skipped_stage_count",
                      std::to_string(command.operation_trace.stages.size()));
            add_field(data, "first_skipped_last_stage",
                      command.operation_trace.stages.empty()
                          ? ""
                          : std::string(world::operation_stage_name(
                                command.operation_trace.stages.back())));
            add_field(data, "first_skipped_mutation_count",
                      std::to_string(command.operation_trace.mutations.size()));
            add_field(data, "first_skipped_replication_dirty",
                      bool_text(command.operation_trace.replication_dirty));
            add_field(data, "first_skipped_save_dirty",
                      bool_text(command.operation_trace.save_dirty));
            break;
        }
    }

    if (report.commands.size() != report.command_delta_count) {
        add_issue(data, InspectionSeverity::error,
                  "world_replication_delta_delivery.command_count_mismatch",
                  "world replication delta delivery command count does not match commands");
    }
    if (materialized_command_count != report.materialized_command_count ||
        skipped_command_count != report.skipped_command_count ||
        sent_message_count != report.sent_message_count) {
        add_issue(data, InspectionSeverity::error,
                  "world_replication_delta_delivery.count_mismatch",
                  "world replication delta delivery aggregate counts do not add up");
    }
    if (report.command_delta_count > 0 && report.sent_message_count == 0) {
        add_issue(data, InspectionSeverity::warning,
                  "world_replication_delta_delivery.no_messages_sent",
                  "world replication delta delivery did not send any typed snapshots");
    }
    if (report.unmatched_relevance_count > 0) {
        add_issue(data, InspectionSeverity::warning,
                  "world_replication_delta_delivery.unmatched_relevance",
                  "world replication delta delivery had relevance reports without matching "
                  "materialized deltas");
    }
    if (report.resync_skipped_count > 0) {
        add_issue(data, InspectionSeverity::warning,
                  "world_replication_delta_delivery.requires_resync",
                  "world replication delta delivery skipped partial deltas that require resync");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const world::WorldReplicationDeltaApplyReport& report) {
    InspectionData data;
    data.object_type = "world_replication_delta_apply_report";
    data.display_name = "World Replication Delta Apply Report";
    data.runtime_id = std::to_string(report.replication_sequence != 0 ? report.replication_sequence
                                                                      : report.command_sequence);
    if (!report.applied) {
        data.state = "not_applied";
    } else if (report.requires_snapshot_resync) {
        data.state = "needs_resync";
    } else if (report.applied_record_count == 0 && report.global_event_count > 0) {
        data.state = "global_events";
    } else if (report.updated_record_count > 0 && report.inserted_record_count > 0) {
        data.state = "mixed";
    } else if (report.updated_record_count > 0) {
        data.state = "updated";
    } else if (report.inserted_record_count > 0) {
        data.state = "inserted";
    } else {
        data.state = "empty";
    }

    const auto inserted_record_count = report.build_pieces_inserted + report.entities_inserted +
                                       report.cargo_inserted + report.inventories_inserted +
                                       report.assemblies_inserted + report.processes_inserted;
    const auto updated_record_count = report.build_pieces_updated + report.entities_updated +
                                      report.cargo_updated + report.inventories_updated +
                                      report.assemblies_updated + report.processes_updated;
    const auto applied_record_count = inserted_record_count + updated_record_count;

    add_field(data, "command_sequence", std::to_string(report.command_sequence));
    add_field(data, "replication_sequence",
              std::to_string(report.replication_sequence != 0 ? report.replication_sequence
                                                              : report.command_sequence));
    add_field(data, "source_client_id", net_id_text(report.source_client_id));
    add_field(data, "command_type", report.command_type);
    add_field(data, "event_count", std::to_string(report.event_count));
    add_field(data, "global_event_count", std::to_string(report.global_event_count));
    add_field(data, "planned_record_count", std::to_string(report.planned_record_count));
    add_field(data, "applied_record_count", std::to_string(report.applied_record_count));
    add_field(data, "inserted_record_count", std::to_string(report.inserted_record_count));
    add_field(data, "updated_record_count", std::to_string(report.updated_record_count));
    add_field(data, "build_pieces_inserted", std::to_string(report.build_pieces_inserted));
    add_field(data, "build_pieces_updated", std::to_string(report.build_pieces_updated));
    add_field(data, "entities_inserted", std::to_string(report.entities_inserted));
    add_field(data, "entities_updated", std::to_string(report.entities_updated));
    add_field(data, "cargo_inserted", std::to_string(report.cargo_inserted));
    add_field(data, "cargo_updated", std::to_string(report.cargo_updated));
    add_field(data, "inventories_inserted", std::to_string(report.inventories_inserted));
    add_field(data, "inventories_updated", std::to_string(report.inventories_updated));
    add_field(data, "assemblies_inserted", std::to_string(report.assemblies_inserted));
    add_field(data, "assemblies_updated", std::to_string(report.assemblies_updated));
    add_field(data, "processes_inserted", std::to_string(report.processes_inserted));
    add_field(data, "processes_updated", std::to_string(report.processes_updated));
    add_field(data, "dirty_region_count_before", std::to_string(report.dirty_region_count_before));
    add_field(data, "dirty_region_count_after", std::to_string(report.dirty_region_count_after));
    add_field(data, "applied", bool_text(report.applied));
    add_field(data, "requires_snapshot_resync", bool_text(report.requires_snapshot_resync));

    if (inserted_record_count != report.inserted_record_count ||
        updated_record_count != report.updated_record_count ||
        applied_record_count != report.applied_record_count ||
        report.inserted_record_count + report.updated_record_count != report.applied_record_count) {
        add_issue(data, InspectionSeverity::error, "world_replication_delta_apply.count_mismatch",
                  "world replication delta apply aggregate counts do not add up");
    }
    if (report.applied && report.applied_record_count != report.planned_record_count) {
        add_issue(
            data, InspectionSeverity::error, "world_replication_delta_apply.planned_count_mismatch",
            "world replication delta apply count does not match planned materialized records");
    }
    if (!report.applied) {
        add_issue(data, InspectionSeverity::warning, "world_replication_delta_apply.not_applied",
                  "world replication delta apply report has not been marked applied");
    }
    if (report.requires_snapshot_resync) {
        add_issue(data, InspectionSeverity::warning,
                  "world_replication_delta_apply.requires_resync",
                  "world replication delta apply report needs snapshot/resync fallback");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const world::WorldClientReplicationApplyReport& report) {
    InspectionData data;
    data.object_type = "world_client_replication_apply_report";
    data.display_name = "World Client Replication Apply Report";
    if (report.drained_batch_count == 0 && report.delta_snapshot_count == 0) {
        data.state = "empty";
    } else if (report.pending_delta_count > 0) {
        data.state = "pending";
    } else if (report.unmatched_delta_count > 0 && report.applied_delta_count > 0) {
        data.state = "partial";
    } else if (report.applied_delta_count > 0) {
        data.state = "applied";
    } else if (report.observed_event_only_count > 0) {
        data.state = "observed";
    } else {
        data.state = "no_batches";
    }

    std::uint32_t matched_delta_count = 0;
    std::uint32_t applied_delta_count = 0;
    std::uint32_t pending_delta_count = 0;
    std::uint32_t observed_event_only_count = 0;
    std::uint32_t total_event_count = 0;
    std::uint32_t total_applied_record_count = 0;
    for (const auto& batch : report.batches) {
        total_event_count += batch.event_count;
        if (batch.has_delta_snapshot) {
            ++matched_delta_count;
        }
        if (batch.applied_delta) {
            ++applied_delta_count;
            total_applied_record_count += batch.delta_apply_report.applied_record_count;
        } else if (batch.state == "pending_delta") {
            ++pending_delta_count;
        } else if (batch.state == "observed_event_only" || batch.state == "empty_events") {
            ++observed_event_only_count;
        }
    }

    add_field(data, "drained_batch_count", std::to_string(report.drained_batch_count));
    add_field(data, "delta_snapshot_count", std::to_string(report.delta_snapshot_count));
    add_field(data, "matched_delta_count", std::to_string(report.matched_delta_count));
    add_field(data, "applied_delta_count", std::to_string(report.applied_delta_count));
    add_field(data, "pending_delta_count", std::to_string(report.pending_delta_count));
    add_field(data, "observed_event_only_count", std::to_string(report.observed_event_only_count));
    add_field(data, "unmatched_delta_count", std::to_string(report.unmatched_delta_count));
    add_field(data, "total_event_count", std::to_string(report.total_event_count));
    add_field(data, "total_applied_record_count",
              std::to_string(report.total_applied_record_count));
    add_field(data, "batch_report_count", std::to_string(report.batches.size()));
    if (!report.batches.empty()) {
        const auto& first = report.batches.front();
        add_field(data, "first_batch_sequence", std::to_string(first.command_sequence));
        add_field(data, "first_batch_command_type", first.command_type);
        add_field(data, "first_batch_state", first.state);
        add_field(data, "first_batch_event_count", std::to_string(first.event_count));
        add_field(data, "first_batch_has_delta_snapshot", bool_text(first.has_delta_snapshot));
        add_field(data, "first_batch_applied_delta", bool_text(first.applied_delta));
        add_field(data, "first_batch_skip_reason", first.skip_reason);
    }

    if (report.batches.size() != report.drained_batch_count ||
        matched_delta_count != report.matched_delta_count ||
        applied_delta_count != report.applied_delta_count ||
        pending_delta_count != report.pending_delta_count ||
        observed_event_only_count != report.observed_event_only_count ||
        total_event_count != report.total_event_count ||
        total_applied_record_count != report.total_applied_record_count ||
        report.matched_delta_count + report.unmatched_delta_count != report.delta_snapshot_count) {
        add_issue(data, InspectionSeverity::error, "world_client_replication_apply.count_mismatch",
                  "world client replication apply aggregate counts do not add up");
    }
    if (report.pending_delta_count > 0) {
        add_issue(data, InspectionSeverity::warning, "world_client_replication_apply.pending_delta",
                  "client replication has subject event batches waiting for typed deltas");
    }
    if (report.unmatched_delta_count > 0) {
        add_issue(data, InspectionSeverity::warning,
                  "world_client_replication_apply.unmatched_delta",
                  "decoded typed deltas were not matched to queued client event batches");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const world::WorldReplicationInterestReport& report) {
    InspectionData data;
    data.object_type = "world_replication_interest_report";
    data.display_name = "World Replication Interest Report";

    const auto visible_subject_total = world_interest_visible_subject_total(report.viewer_reports);
    const auto excluded_lod_subject_total =
        world_interest_excluded_lod_subject_total(report.viewer_reports);
    const auto skipped_non_saved_subject_total =
        world_interest_skipped_non_saved_subject_total(report.viewer_reports);

    if (report.viewer_count == 0) {
        data.state = "no_viewers";
    } else if (visible_subject_total == 0) {
        data.state = "filtered";
    } else {
        data.state = "active";
    }

    add_field(data, "subject_count", std::to_string(report.subject_count));
    add_field(data, "saved_subject_count", std::to_string(report.saved_subject_count));
    add_field(data, "non_saved_subject_count", std::to_string(report.non_saved_subject_count));
    add_field(data, "viewer_count", std::to_string(report.viewer_count));
    add_field(data, "viewer_report_count", std::to_string(report.viewer_reports.size()));
    add_field(data, "policy_rule_count", std::to_string(report.policy.client_rules.size()));
    add_field(data, "broadcast_by_default", bool_text(report.broadcast_by_default));
    add_field(data, "receives_global_events", bool_text(report.receives_global_events));
    add_field(data, "include_full", bool_text(report.include_full));
    add_field(data, "include_simplified", bool_text(report.include_simplified));
    add_field(data, "include_sleeping", bool_text(report.include_sleeping));
    add_field(data, "include_unloaded", bool_text(report.include_unloaded));
    add_field(data, "visible_subject_total", std::to_string(visible_subject_total));
    add_field(data, "excluded_lod_subject_total", std::to_string(excluded_lod_subject_total));
    add_field(data, "skipped_non_saved_subject_total",
              std::to_string(skipped_non_saved_subject_total));

    if (!report.viewer_reports.empty()) {
        const auto& first_viewer = report.viewer_reports.front();
        add_field(data, "first_viewer_id", net_id_text(first_viewer.viewer_id));
        add_field(data, "first_viewer_visible_subject_count",
                  std::to_string(first_viewer.visible_subject_count));
        add_field(data, "first_viewer_excluded_lod_subject_count",
                  std::to_string(first_viewer.excluded_lod_subject_count));
        add_field(data, "first_viewer_skipped_non_saved_subject_count",
                  std::to_string(first_viewer.skipped_non_saved_subject_count));
        if (!first_viewer.visible_subjects.empty()) {
            add_field(data, "first_visible_subject_id",
                      save_id_text(first_viewer.visible_subjects.front()));
        }
    }

    if (report.subject_count != report.saved_subject_count + report.non_saved_subject_count) {
        add_issue(data, InspectionSeverity::error, "world_replication_interest.subject_mismatch",
                  "world replication interest subject counts do not add up");
    }
    if (report.viewer_count != report.viewer_reports.size()) {
        add_issue(data, InspectionSeverity::error, "world_replication_interest.viewer_mismatch",
                  "world replication interest viewer count does not match reports");
    }
    if (report.policy.client_rules.size() != report.viewer_reports.size()) {
        add_issue(data, InspectionSeverity::error, "world_replication_interest.policy_mismatch",
                  "world replication interest policy rules do not match viewer reports");
    }
    if (report.subject_count > 0 && report.viewer_count > 0 && visible_subject_total == 0) {
        add_issue(data, InspectionSeverity::warning,
                  "world_replication_interest.no_visible_subjects",
                  "world replication interest derived no visible saved subjects");
    }
    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const world::WorldOperation& operation) {
    InspectionData data;
    data.object_type = "world_operation";
    data.display_name = operation.name();
    if (operation.is_committed()) {
        data.state = "committed";
    } else if (operation.is_rolled_back()) {
        data.state = "rolled_back";
    } else if (operation.has_failed()) {
        data.state = "failed";
    } else {
        data.state = "open";
    }

    const auto& stages = operation.stages();
    add_field(data, "stage_count", std::to_string(stages.size()));
    add_field(data, "stage_sequence", operation_stage_sequence(stages));
    add_field(data, "last_stage",
              stages.empty() ? "" : std::string(world::operation_stage_name(stages.back())));
    add_field(data, "reserved_id_count", std::to_string(operation.reserved_ids().size()));
    add_field(data, "mutation_count", std::to_string(operation.mutations().size()));
    add_field(data, "derived_update_count", std::to_string(operation.derived_updates().size()));
    add_field(data, "event_count", std::to_string(operation.events().size()));
    add_field(data, "replication_dirty", bool_text(operation.replication_dirty()));
    add_field(data, "save_dirty", bool_text(operation.save_dirty()));

    if (operation.has_failed()) {
        const auto& failure = operation.failure();
        add_issue(data, InspectionSeverity::error, failure.code, failure.message);
    } else if (!operation.is_committed() && !operation.mutations().empty() &&
               !operation.save_dirty()) {
        add_issue(data, InspectionSeverity::warning, "operation.save_not_dirty",
                  "operation has mutations but save data is not marked dirty");
    } else if (!operation.is_committed() && !operation.mutations().empty() &&
               !operation.replication_dirty()) {
        add_issue(data, InspectionSeverity::warning, "operation.replication_not_dirty",
                  "operation has mutations but replication state is not marked dirty");
    }

    return data;
}

InspectionData Inspector::inspect(const world::WorldState& state) {
    InspectionData data;
    const auto stats = state.stats();
    const auto& metadata = state.metadata();

    data.object_type = "world_state";
    data.display_name = "World State";

    add_field(data, "schema_version", std::to_string(metadata.schema_version));
    add_field(data, "game_version", metadata.game_version);
    add_field(data, "world_seed", std::to_string(metadata.world_seed));
    add_field(data, "world_time", std::to_string(state.world_time()));
    add_field(data, "voxel_palette_count",
              std::to_string(state.voxel_palette_manifest().entries.size()));
    add_field(data, "enabled_mod_count", std::to_string(metadata.enabled_mods.size()));
    add_field(data, "migration_count", std::to_string(metadata.migration_history.size()));
    add_field(data, "next_save_id", state.save_ids().peek_next().to_string());
    add_field(data, "next_runtime_handle", state.runtime_handles().peek_next().to_string());
    add_field(data, "next_entity_net_id", state.entity_net_ids().peek_next().to_string());
    add_field(data, "next_process_id", state.process_ids().peek_next().to_string());
    add_field(data, "chunk_count", std::to_string(stats.chunk_count));
    add_field(data, "region_count", std::to_string(stats.region_count));
    add_field(data, "region_connection_count", std::to_string(stats.region_connection_count));
    add_field(data, "dirty_region_count", std::to_string(stats.dirty_region_count));
    add_dirty_region_count_fields(data, state.dirty_regions());
    add_field(data, "build_object_count", std::to_string(stats.build_object_count));
    add_field(data, "entity_count", std::to_string(stats.entity_count));
    add_field(data, "persistent_entity_count", std::to_string(stats.persistent_entity_count));
    add_field(data, "cargo_count", std::to_string(stats.cargo_count));
    add_field(data, "inventory_count", std::to_string(stats.inventory_count));
    add_field(data, "workpiece_count", std::to_string(stats.workpiece_count));
    add_field(data, "physical_resource_count", std::to_string(stats.physical_resource_count));
    add_field(data, "assembly_count", std::to_string(stats.assembly_count));
    add_field(data, "process_count", std::to_string(stats.process_count));
    add_field(data, "room_count", std::to_string(stats.room_count));
    add_field(data, "network_count", std::to_string(stats.network_count));
    add_field(data, "mod_state_count", std::to_string(stats.mod_state_count));
    add_field(data, "missing_prototype_count", std::to_string(stats.missing_prototype_count));
    add_field(data, "fire_count", std::to_string(stats.fire_count));

    add_status_issue(data, metadata.validate());
    data.state = data.has_errors() ? "invalid" : has_world_content(stats) ? "loaded" : "empty";
    return data;
}

InspectionData Inspector::inspect(const save::SaveDatabaseStats& stats) {
    InspectionData data;
    data.object_type = "save_database_stats";
    data.display_name = "Save Database";
    const bool has_pending_snapshot_journal =
        stats.journal_highest_sequence > stats.journal_checkpoint_sequence;
    const bool has_pending_chunk_delta_journal = stats.chunk_delta_journal_entry_count > 0;
    const bool has_pending_journal =
        has_pending_snapshot_journal || has_pending_chunk_delta_journal;
    if (has_pending_journal) {
        data.state = "journal_pending";
    } else if (!stats.has_snapshot) {
        data.state = "empty";
    } else if (stats.uses_generation_manifest) {
        data.state = "generation";
    } else {
        data.state = "legacy";
    }

    add_field(data, "layout", stats.uses_generation_manifest ? "generation" : "legacy");
    add_field(data, "uses_generation_manifest", bool_text(stats.uses_generation_manifest));
    add_field(data, "active_generation", stats.active_generation);
    add_field(data, "committed_generation_count", std::to_string(stats.committed_generation_count));
    add_field(data, "staged_generation_count", std::to_string(stats.staged_generation_count));
    add_field(data, "stale_generation_count", std::to_string(stats.stale_generation_count));
    add_field(data, "has_snapshot", bool_text(stats.has_snapshot));
    add_field(data, "snapshot_bytes", std::to_string(stats.snapshot_bytes));
    add_field(data, "chunk_delta_count", std::to_string(stats.chunk_delta_count));
    add_field(data, "chunk_delta_bytes", std::to_string(stats.chunk_delta_bytes));
    add_field(data, "chunk_delta_journal_entry_count",
              std::to_string(stats.chunk_delta_journal_entry_count));
    add_field(data, "chunk_delta_journal_bytes", std::to_string(stats.chunk_delta_journal_bytes));
    add_field(data, "chunk_delta_journal_highest_sequence",
              std::to_string(stats.chunk_delta_journal_highest_sequence));
    add_field(data, "journal_entry_count", std::to_string(stats.journal_entry_count));
    add_field(data, "journal_bytes", std::to_string(stats.journal_bytes));
    add_field(data, "journal_checkpoint_sequence",
              std::to_string(stats.journal_checkpoint_sequence));
    add_field(data, "journal_highest_sequence", std::to_string(stats.journal_highest_sequence));

    if (!stats.has_snapshot && !has_pending_journal) {
        add_issue(data, InspectionSeverity::warning, "save_database.empty",
                  "save database does not contain an active snapshot");
    }
    if (has_pending_snapshot_journal) {
        add_issue(data, InspectionSeverity::warning, "save_database.journal_pending",
                  "save database has a durable journal entry awaiting background compaction");
    }
    if (has_pending_chunk_delta_journal) {
        add_issue(data, InspectionSeverity::warning, "save_database.chunk_delta_journal_pending",
                  "save database has durable chunk delta entries awaiting compaction");
    }
    if (stats.uses_generation_manifest && stats.active_generation.empty()) {
        add_issue(data, InspectionSeverity::error, "save_database.missing_active_generation",
                  "save database generation manifest did not report an active generation");
    }
    if (stats.staged_generation_count > 0) {
        add_issue(data, InspectionSeverity::warning, "save_database.staged_generations",
                  "save database contains staged generation directories");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const save::SaveDatabaseMaintenanceResult& result) {
    InspectionData data;
    data.object_type = "save_database_maintenance";
    data.display_name = "Save Database Maintenance";
    data.state = result.changed() ? "changed" : "unchanged";

    add_field(data, "changed", bool_text(result.changed()));
    add_field(data, "recovered_staged_generation_count",
              std::to_string(result.recovered_staged_generation_count));
    add_field(data, "discarded_journal_temporary_entry_count",
              std::to_string(result.journal_recovery.discarded_temporary_entry_count));
    add_field(data, "journal_compacted", bool_text(result.journal_recovery.compaction.compacted));
    add_field(data, "journal_compacted_sequence",
              std::to_string(result.journal_recovery.compaction.compacted_sequence));
    add_field(data, "removed_journal_entry_count",
              std::to_string(result.journal_recovery.compaction.removed_entry_count));
    add_field(data, "discarded_chunk_delta_journal_temporary_entry_count",
              std::to_string(result.chunk_delta_journal_recovery.discarded_temporary_entry_count));
    add_field(data, "discarded_compacted_chunk_delta_journal_directory",
              bool_text(result.chunk_delta_journal_recovery.discarded_compacted_directory));
    add_field(data, "chunk_delta_journal_compacted",
              bool_text(result.chunk_delta_journal_compaction.compacted));
    add_field(data, "chunk_delta_journal_merged_entry_count",
              std::to_string(result.chunk_delta_journal_compaction.merged_entry_count));
    add_field(data, "chunk_delta_journal_removed_entry_count",
              std::to_string(result.chunk_delta_journal_compaction.removed_entry_count));
    add_field(data, "pruned_stale_generation_count",
              std::to_string(result.pruned_stale_generation_count));
    add_field(data, "compacted_chunk_delta_count",
              std::to_string(result.compacted_chunk_delta_count));
    add_field(data, "before_active_generation", result.before.active_generation);
    add_field(data, "after_active_generation", result.after.active_generation);
    add_field(data, "before_committed_generation_count",
              std::to_string(result.before.committed_generation_count));
    add_field(data, "after_committed_generation_count",
              std::to_string(result.after.committed_generation_count));
    add_field(data, "before_staged_generation_count",
              std::to_string(result.before.staged_generation_count));
    add_field(data, "after_staged_generation_count",
              std::to_string(result.after.staged_generation_count));
    add_field(data, "before_stale_generation_count",
              std::to_string(result.before.stale_generation_count));
    add_field(data, "after_stale_generation_count",
              std::to_string(result.after.stale_generation_count));
    add_field(data, "before_chunk_delta_count", std::to_string(result.before.chunk_delta_count));
    add_field(data, "after_chunk_delta_count", std::to_string(result.after.chunk_delta_count));

    if (result.before.has_snapshot && !result.after.has_snapshot) {
        add_issue(data, InspectionSeverity::error, "save_database_maintenance.lost_snapshot",
                  "save database maintenance removed the active snapshot");
    }
    if (result.after.staged_generation_count > 0) {
        add_issue(data, InspectionSeverity::warning,
                  "save_database_maintenance.remaining_staged_generations",
                  "save database still contains staged generation directories after maintenance");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const save::SaveDatabaseMigrationResult& result) {
    InspectionData data;
    data.object_type = "save_database_migration";
    data.display_name = "Save Database Migration";
    data.state = result.changed() ? "migrated" : "unchanged";

    add_field(data, "changed", bool_text(result.changed()));
    add_field(data, "wrote_snapshot", bool_text(result.wrote_snapshot));
    add_field(data, "previous_schema_version",
              std::to_string(result.migration.previous_schema_version));
    add_field(data, "final_schema_version", std::to_string(result.migration.final_schema_version));
    add_field(data, "applied_migration_count",
              std::to_string(result.migration.applied_migrations.size()));
    add_field(data, "before_active_generation", result.before.active_generation);
    add_field(data, "after_active_generation", result.after.active_generation);
    add_field(data, "before_committed_generation_count",
              std::to_string(result.before.committed_generation_count));
    add_field(data, "after_committed_generation_count",
              std::to_string(result.after.committed_generation_count));
    add_field(data, "before_stale_generation_count",
              std::to_string(result.before.stale_generation_count));
    add_field(data, "after_stale_generation_count",
              std::to_string(result.after.stale_generation_count));

    if (!result.migration.applied_migrations.empty() && !result.wrote_snapshot) {
        add_issue(data, InspectionSeverity::error, "save_database_migration.not_persisted",
                  "save database migration applied in memory but did not write a new snapshot");
    }
    if (result.before.has_snapshot && !result.after.has_snapshot) {
        add_issue(data, InspectionSeverity::error, "save_database_migration.lost_snapshot",
                  "save database migration removed the active snapshot");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const save::SaveSlotCatalogSummary& catalog) {
    InspectionData data;
    data.object_type = "save_slot_catalog";
    data.display_name = "Save Slot Catalog";

    auto empty_slot_count = std::size_t{0};
    auto generation_slot_count = std::size_t{0};
    auto legacy_slot_count = std::size_t{0};
    auto invalid_slot_count = std::size_t{0};
    auto staged_generation_count = std::size_t{0};
    auto chunk_delta_count = std::size_t{0};

    for (const auto& slot : catalog.slots) {
        const auto slot_inspection = inspect(slot);
        if (slot_inspection.has_errors()) {
            ++invalid_slot_count;
        }
        if (!slot.database_stats.has_snapshot) {
            ++empty_slot_count;
        } else if (slot.database_stats.uses_generation_manifest) {
            ++generation_slot_count;
        } else {
            ++legacy_slot_count;
        }
        staged_generation_count += slot.database_stats.staged_generation_count;
        chunk_delta_count += slot.database_stats.chunk_delta_count;
    }

    add_field(data, "root", catalog.root.generic_string());
    add_field(data, "slot_count", std::to_string(catalog.slots.size()));
    add_field(data, "empty_slot_count", std::to_string(empty_slot_count));
    add_field(data, "generation_slot_count", std::to_string(generation_slot_count));
    add_field(data, "legacy_slot_count", std::to_string(legacy_slot_count));
    add_field(data, "invalid_slot_count", std::to_string(invalid_slot_count));
    add_field(data, "staged_generation_count", std::to_string(staged_generation_count));
    add_field(data, "chunk_delta_count", std::to_string(chunk_delta_count));

    if (catalog.root.empty()) {
        add_issue(data, InspectionSeverity::error, "save_slot_catalog.empty_root",
                  "save slot catalog summary has no root path");
    }
    if (catalog.slots.empty()) {
        add_issue(data, InspectionSeverity::warning, "save_slot_catalog.empty",
                  "save slot catalog does not contain visible slots");
    }
    if (invalid_slot_count > 0) {
        add_issue(data, InspectionSeverity::error, "save_slot_catalog.invalid_slots",
                  "save slot catalog contains invalid slot summaries");
    }
    if (staged_generation_count > 0) {
        add_issue(data, InspectionSeverity::warning, "save_slot_catalog.staged_generations",
                  "save slot catalog contains staged generation directories");
    }

    data.state = data.has_errors() ? "invalid" : catalog.slots.empty() ? "empty" : "loaded";
    return data;
}

InspectionData Inspector::inspect(const save::SaveSlotSummary& slot) {
    InspectionData data;
    data.object_type = "save_slot";
    data.display_name =
        slot.metadata.display_name.empty() ? slot.slot_id : slot.metadata.display_name;
    data.runtime_id = slot.slot_id;
    if (!slot.database_stats.has_snapshot) {
        data.state = "empty";
    } else if (slot.database_stats.uses_generation_manifest) {
        data.state = "generation";
    } else {
        data.state = "legacy";
    }

    add_field(data, "slot_id", slot.slot_id);
    add_field(data, "display_name", slot.metadata.display_name);
    add_field(data, "created_at_ms", std::to_string(slot.metadata.created_at_ms));
    add_field(data, "last_played_at_ms", std::to_string(slot.metadata.last_played_at_ms));
    add_field(data, "last_saved_at_ms", std::to_string(slot.metadata.last_saved_at_ms));
    add_field(data, "generator_preset", slot.generator_preset);
    add_field(data, "generator_version", slot.generator_version);
    add_field(data, "path", slot.path.generic_string());
    add_field(data, "layout",
              slot.database_stats.uses_generation_manifest ? "generation" : "legacy");
    add_field(data, "has_snapshot", bool_text(slot.database_stats.has_snapshot));
    add_field(data, "active_generation", slot.database_stats.active_generation);
    add_field(data, "committed_generation_count",
              std::to_string(slot.database_stats.committed_generation_count));
    add_field(data, "staged_generation_count",
              std::to_string(slot.database_stats.staged_generation_count));
    add_field(data, "stale_generation_count",
              std::to_string(slot.database_stats.stale_generation_count));
    add_field(data, "chunk_delta_count", std::to_string(slot.database_stats.chunk_delta_count));

    if (slot.slot_id.empty()) {
        add_issue(data, InspectionSeverity::error, "save_slot.empty_id",
                  "save slot summary has no slot id");
    }
    if (slot.validation_error.has_value()) {
        add_issue(data, InspectionSeverity::error, slot.validation_error->code,
                  slot.validation_error->message);
    }
    add_status_issue(data, slot.metadata.validate());
    if (!slot.database_stats.has_snapshot) {
        add_issue(data, InspectionSeverity::warning, "save_slot.empty",
                  "save slot does not contain an active snapshot");
    }
    if (slot.database_stats.staged_generation_count > 0) {
        add_issue(data, InspectionSeverity::warning, "save_slot.staged_generations",
                  "save slot contains staged generation directories");
    }

    if (data.has_errors()) {
        data.state = "invalid";
    }
    return data;
}

InspectionData Inspector::inspect(const save::SaveSnapshot& snapshot,
                                  const modding::PrototypeRegistry* prototypes) {
    InspectionData data;
    data.object_type = "save_snapshot";
    data.display_name = "Save Snapshot";
    data.state = "loaded";
    add_field(data, "schema_version", std::to_string(snapshot.metadata.schema_version));
    add_field(data, "game_version", snapshot.metadata.game_version);
    add_field(data, "world_seed", std::to_string(snapshot.metadata.world_seed));
    add_field(data, "world_time", std::to_string(snapshot.metadata.world_time));
    add_field(data, "mod_count", std::to_string(snapshot.metadata.enabled_mods.size()));
    add_field(data, "voxel_palette_count", std::to_string(snapshot.voxel_palette.entries.size()));
    add_field(data, "chunk_edit_count", std::to_string(snapshot.chunk_edits.size()));
    add_field(data, "build_piece_count", std::to_string(snapshot.build_pieces.size()));
    add_field(data, "entity_count", std::to_string(snapshot.entities.size()));
    add_field(data, "inventory_count", std::to_string(snapshot.inventories.size()));
    add_field(data, "cargo_count", std::to_string(snapshot.cargo_records.size()));
    add_field(data, "workpiece_count", std::to_string(snapshot.workpieces.size()));
    add_field(data, "assembly_count", std::to_string(snapshot.assemblies.size()));
    add_field(data, "process_count", std::to_string(snapshot.processes.size()));
    add_field(data, "mod_state_count", std::to_string(snapshot.mod_states.size()));
    add_field(data, "missing_prototype_count", std::to_string(snapshot.missing_prototypes.size()));
    add_field(data, "fire_count", std::to_string(snapshot.fires.size()));

    add_status_issue(data, snapshot.metadata.validate());
    if (prototypes != nullptr) {
        const auto validation = save::SaveSnapshotValidator::validate(snapshot, *prototypes);
        for (const auto& issue : validation.issues) {
            add_issue(data, InspectionSeverity::error, issue.code, issue.message);
        }
        data.state = validation.valid() ? "valid" : "invalid";
    }

    return data;
}

std::string Inspector::render_text(const InspectionData& data) {
    std::ostringstream output;
    output << data.object_type << ": " << data.display_name << '\n';
    if (!data.prototype_id.empty()) {
        output << "prototype_id=" << data.prototype_id << '\n';
    }
    if (!data.save_id.empty()) {
        output << "save_id=" << data.save_id << '\n';
    }
    if (!data.runtime_id.empty()) {
        output << "runtime_id=" << data.runtime_id << '\n';
    }
    if (!data.state.empty()) {
        output << "state=" << data.state << '\n';
    }
    for (const auto& field : data.fields) {
        output << field.name << '=' << field.value << '\n';
    }
    for (const auto& issue : data.issues) {
        output << severity_text(issue.severity) << ':' << issue.code << '=' << issue.message
               << '\n';
    }
    return output.str();
}

} // namespace heartstead::debug
