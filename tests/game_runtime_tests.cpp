#include "engine/content/content_validation.hpp"
#include "engine/modding/mod_validation.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/net/command_payload.hpp"
#include "engine/scripting/script_host_event.hpp"
#include "engine/scripting/script_runtime.hpp"
#include "engine/world/voxel_change.hpp"
#include "engine/world/world_commands.hpp"
#include "engine/world/world_state.hpp"
#include "game/runtime/game_inspection.hpp"
#include "game/runtime/game_runtime.hpp"
#include "game/runtime/script_host_commands.hpp"

#include <cassert>
#include <filesystem>
#include <string>
#include <utility>

namespace {

std::filesystem::path source_root() {
    return std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR);
}

void test_game_runtime_starts_from_base_mod() {
    const auto report = heartstead::modding::ModValidation::validate(source_root() / "mods");
    assert(!report.has_errors());

    auto runtime =
        heartstead::game::GameRuntime::initialize(heartstead::game::GameRuntimeConfig{}, report);
    assert(runtime);
    assert(runtime.value().is_initialized());
    assert(runtime.value().startup_report().base_mod_loaded);
    assert(runtime.value().startup_report().mod_count == 1);
    assert(runtime.value().startup_report().prototype_count == report.registry.size());
    assert(runtime.value().startup_report().selected_scenario_id == "base:scenarios/homestead");
    assert(runtime.value().startup_report().script_host_api_count ==
           heartstead::game::GameRuntime::default_script_host_apis().size());
    assert(runtime.value().startup_report().script_host_apis.size() ==
           runtime.value().startup_report().script_host_api_count);
    assert(heartstead::scripting::validate_script_host_api_registry(
        runtime.value().startup_report().script_host_apis));
    assert(heartstead::scripting::find_script_host_api(
               runtime.value().startup_report().script_host_apis, "world.set_voxel") != nullptr);
    assert(heartstead::scripting::find_script_host_api(
               runtime.value().startup_report().script_host_apis, "ui.notify") != nullptr);

    const auto script_runtime_desc =
        runtime.value().make_script_runtime_desc(heartstead::scripting::ScriptBackend::luau);
    assert(script_runtime_desc.backend == heartstead::scripting::ScriptBackend::luau);
    assert(script_runtime_desc.host_apis.size() ==
           runtime.value().startup_report().script_host_api_count);
    assert(runtime.value().startup_report().script_host_command_route_count ==
           heartstead::game::GameRuntime::default_script_host_command_routes().size());
    assert(heartstead::game::validate_script_host_command_routes(
        runtime.value().startup_report().script_host_command_routes));

    auto command_router = runtime.value().make_script_host_command_router();
    assert(command_router.route_count() ==
           runtime.value().startup_report().script_host_command_route_count);
    const auto* set_voxel_route = command_router.find_route("world.set_voxel");
    assert(set_voxel_route != nullptr);
    const auto* set_voxel_api = heartstead::scripting::find_script_host_api(
        runtime.value().startup_report().script_host_apis, "world.set_voxel");
    assert(set_voxel_api != nullptr);
    assert(set_voxel_api->arguments.size() == 3);
    assert(set_voxel_api->arguments[0].name == "chunk");
    assert(set_voxel_api->arguments[0].kind == heartstead::scripting::ScriptValueKind::string);
    assert(command_router.find_route("ui.notify") == nullptr);
    auto route_inspection = heartstead::game::GameInspector::inspect(*set_voxel_route);
    assert(route_inspection.object_type == "script_host_command_route");
    assert(route_inspection.state == "runtime_server");
    assert(route_inspection.find_field("api_id")->value == "world.set_voxel");
    assert(route_inspection.find_field("command_type")->value == "world.set_voxel");
    assert(route_inspection.find_field("required_argument_count")->value == "3");
    assert(route_inspection.find_field("required_arguments")->value == "chunk,voxel,prototype");
    assert(route_inspection.issues.empty());

    heartstead::scripting::ScriptHostEvent set_voxel_event;
    set_voxel_event.sequence = 1;
    set_voxel_event.api_id = "world.set_voxel";
    set_voxel_event.module_id = "base:scripts/runtime_server/tick";
    set_voxel_event.source_mod_id = "base";
    set_voxel_event.source_path = "mods/base/scripts/runtime_server/tick.luau";
    set_voxel_event.stage = heartstead::scripting::ScriptStage::runtime_server;
    set_voxel_event.function_name = "tick";
    set_voxel_event.arguments = {
        heartstead::scripting::ScriptValue::string("1099511627776|0|-1099511627776"),
        heartstead::scripting::ScriptValue::string("1|2|3"),
        heartstead::scripting::ScriptValue::string("base:voxels/test")};
    auto command_envelopes = command_router.make_command_envelopes(
        {set_voxel_event}, heartstead::game::ScriptHostCommandDispatchDesc{
                               heartstead::core::NetId::from_value(99), 700, 1234});
    assert(command_envelopes);
    assert(command_envelopes.value().size() == 1);
    assert(command_envelopes.value()[0].sequence == 700);
    assert(command_envelopes.value()[0].sender == heartstead::core::NetId::from_value(99));
    assert(command_envelopes.value()[0].type == "world.set_voxel");
    assert(command_envelopes.value()[0].client_time_ms == 1234);
    auto decoded_payload =
        heartstead::net::CommandPayloadTextCodec::decode(command_envelopes.value()[0].payload);
    assert(decoded_payload);
    assert(*decoded_payload.value().find("chunk") == "1099511627776|0|-1099511627776");
    assert(*decoded_payload.value().find("voxel") == "1|2|3");
    assert(*decoded_payload.value().find("prototype") == "base:voxels/test");

    heartstead::net::ServerCommandDispatcher dispatcher;
    assert(heartstead::world::WorldCommandRegistry::register_engine_commands(dispatcher));

    heartstead::world::WorldState world_state;
    heartstead::net::CommandExecutionContext command_context;
    command_context.executor_role = heartstead::net::CommandExecutorRole::authoritative_server;
    command_context.server_time_ms = 5000;
    command_context.world_state = &world_state;
    heartstead::world::VoxelPalette voxel_palette;
    heartstead::world::VoxelDefinition voxel_definition;
    voxel_definition.type = 4;
    voxel_definition.prototype_id =
        heartstead::core::PrototypeId::parse("base:voxels/test").value();
    voxel_definition.display_name = "Test";
    voxel_definition.terrain_material = "test";
    voxel_definition.mining_tool = "none";
    assert(voxel_palette.add(std::move(voxel_definition)));
    command_context.voxel_palette = &voxel_palette;
    world_state.chunks().get_or_create({1099511627776LL, 0, -1099511627776LL}).clear_all_dirty();

    auto dispatched_batch =
        command_router.dispatch_commands({set_voxel_event},
                                         heartstead::game::ScriptHostCommandDispatchDesc{
                                             heartstead::core::NetId::from_value(99), 800, 5678},
                                         dispatcher, command_context);
    assert(dispatched_batch);
    assert(dispatched_batch.value().command_count == 1);
    assert(dispatched_batch.value().success_count == 1);
    assert(dispatched_batch.value().failure_count == 0);
    assert(dispatched_batch.value().reports.size() == 1);
    assert(dispatched_batch.value().reports[0].script_event_sequence == 1);
    assert(dispatched_batch.value().reports[0].command_sequence == 800);
    assert(dispatched_batch.value().reports[0].api_id == "world.set_voxel");
    assert(dispatched_batch.value().reports[0].command_type == "world.set_voxel");
    assert(dispatched_batch.value().reports[0].success);
    assert(dispatched_batch.value().reports[0].committed_world_mutation);
    assert(dispatched_batch.value().reports[0].events.size() == 1);
    assert(dispatched_batch.value().reports[0].events.front().type ==
           heartstead::world::voxel_changed_event_type);
    assert(heartstead::game::validate_script_host_command_batch_result(dispatched_batch.value()));
    auto report_inspection =
        heartstead::game::GameInspector::inspect(dispatched_batch.value().reports[0]);
    assert(report_inspection.object_type == "script_host_command_report");
    assert(report_inspection.state == "accepted");
    assert(report_inspection.find_field("api_id")->value == "world.set_voxel");
    assert(report_inspection.find_field("success")->value == "true");
    assert(report_inspection.find_field("committed_world_mutation")->value == "true");
    assert(report_inspection.find_field("event_count")->value == "1");
    assert(report_inspection.issues.empty());
    auto batch_inspection = heartstead::game::GameInspector::inspect(dispatched_batch.value());
    assert(batch_inspection.object_type == "script_host_command_batch");
    assert(batch_inspection.state == "accepted");
    assert(batch_inspection.find_field("command_count")->value == "1");
    assert(batch_inspection.find_field("success_count")->value == "1");
    assert(batch_inspection.find_field("failure_count")->value == "0");
    assert(batch_inspection.issues.empty());
    auto routed_voxel = world_state.chunks().get({1099511627776LL, 0, -1099511627776LL}, {1, 2, 3});
    assert(routed_voxel);
    assert(routed_voxel.value().type == 4);
    assert(routed_voxel.value().light == 0);

    auto client_context = command_context;
    client_context.executor_role = heartstead::net::CommandExecutorRole::client_prediction;
    auto rejected_batch =
        command_router.dispatch_commands({set_voxel_event},
                                         heartstead::game::ScriptHostCommandDispatchDesc{
                                             heartstead::core::NetId::from_value(99), 900, 5678},
                                         dispatcher, client_context);
    assert(rejected_batch);
    assert(rejected_batch.value().command_count == 1);
    assert(rejected_batch.value().success_count == 0);
    assert(rejected_batch.value().failure_count == 1);
    assert(rejected_batch.value().reports[0].error_code == "command.not_authoritative");
    auto rejected_report_inspection =
        heartstead::game::GameInspector::inspect(rejected_batch.value().reports[0]);
    assert(rejected_report_inspection.object_type == "script_host_command_report");
    assert(rejected_report_inspection.state == "rejected");
    assert(rejected_report_inspection.find_field("success")->value == "false");
    assert(rejected_report_inspection.find_field("error_code")->value ==
           "command.not_authoritative");
    assert(rejected_report_inspection.issues.empty());
    auto rejected_batch_inspection =
        heartstead::game::GameInspector::inspect(rejected_batch.value());
    assert(rejected_batch_inspection.state == "partial_or_rejected");
    assert(rejected_batch_inspection.find_field("failure_count")->value == "1");
    assert(rejected_batch_inspection.issues.empty());

    for (const auto& kind : heartstead::game::GameRuntime::default_required_prototype_kinds()) {
        assert(runtime.value().startup_report().count_kind(kind) > 0);
        assert(runtime.value().require_prototype_kind(kind));
    }

    assert(runtime.value().require_system(heartstead::game::GameSystemKind::building));
    assert(runtime.value().require_system(heartstead::game::GameSystemKind::rooms));
    assert(runtime.value().require_system(heartstead::game::GameSystemKind::workpieces));

    const auto* logistics =
        runtime.value().find_system(heartstead::game::GameSystemKind::logistics);
    assert(logistics != nullptr);
    assert(logistics->server_authoritative);
    assert(logistics->mod_data_driven);
    assert(heartstead::game::game_system_kind_name(heartstead::game::GameSystemKind::magic) ==
           "magic");
}

void test_game_runtime_starts_from_aggregate_content() {
    const auto report = heartstead::content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    const auto drying_id = heartstead::core::PrototypeId::parse("base:processes/drying");
    assert(drying_id.has_value());
    const heartstead::processes::ProcessDefinition* drying = nullptr;
    for (const auto& definition : report.process_definitions) {
        if (definition.prototype_id == drying_id.value()) {
            drying = &definition;
            break;
        }
    }
    assert(drying != nullptr && drying->default_required_work_ticks == 3'600);

    auto runtime =
        heartstead::game::GameRuntime::initialize(heartstead::game::GameRuntimeConfig{}, report);
    assert(runtime);
    assert(runtime.value().is_initialized());
    assert(runtime.value().startup_report().aggregate_content_validated);
    assert(runtime.value().startup_report().base_mod_loaded);
    assert(runtime.value().startup_report().mod_count == report.mods.size());
    assert(runtime.value().startup_report().resource_pack_count == report.resource_packs.size());
    assert(runtime.value().startup_report().prototype_count == report.registry.size());
    assert(runtime.value().startup_report().active_asset_count ==
           report.asset_catalog.active_count());
    assert(runtime.value().startup_report().voxel_palette_entry_count ==
           report.voxel_palette.size());
    assert(runtime.value().startup_report().voxel_palette_entry_count == 9);
    assert(runtime.value().startup_report().material_definition_count ==
           report.material_registry.size());
    assert(runtime.value().startup_report().material_asset_reference_count ==
           report.material_assets.references.size());
    assert(runtime.value().startup_report().sound_event_count == report.sound_events.size());
    assert(runtime.value().startup_report().sound_event_count == 9);
    auto audio = runtime.value().create_audio_system(heartstead::audio::AudioBackend::null_backend);
    assert(audio);
    assert(audio.value()->backend() == heartstead::audio::AudioBackend::null_backend);
    auto playback =
        runtime.value().create_audio_system(heartstead::audio::AudioBackend::miniaudio, {}, true);
    assert(playback);
    assert(playback.value()->device_state() == heartstead::audio::AudioDeviceState::running);
    auto ambient_id = heartstead::core::PrototypeId::parse("base:audio/homestead_ambient").value();
    auto ambient = playback.value()->play({ambient_id, std::nullopt, 1.0F, 1.0F});
    assert(ambient);
    auto footstep_id = heartstead::core::PrototypeId::parse("base:audio/earth_footstep").value();
    heartstead::audio::AudioEmitterState emitter;
    auto footstep = playback.value()->play({footstep_id, emitter, 1.0F, 1.0F});
    assert(footstep);
    assert(playback.value()->update(0.01F));
    assert(playback.value()->stop(footstep.value()));
    assert(playback.value()->stop(ambient.value()));
    assert(runtime.value().require_prototype_kind(heartstead::modding::PrototypeKinds::material));
    assert(runtime.value().require_prototype_kind(heartstead::modding::PrototypeKinds::scenario));
    assert(runtime.value().startup_report().selected_scenario_id == "base:scenarios/homestead");
    assert(runtime.value().startup_report().selected_scenario_start_region == "temperate_valley");
    assert(runtime.value().startup_report().selected_scenario_spawn_mode == "homestead");
    assert(runtime.value().startup_report().script_host_api_count ==
           heartstead::game::GameRuntime::default_script_host_apis().size());
    assert(runtime.value().startup_report().script_host_command_route_count ==
           heartstead::game::GameRuntime::default_script_host_command_routes().size());
}

void test_game_runtime_accepts_additional_script_host_api() {
    const auto report = heartstead::modding::ModValidation::validate(source_root() / "mods");
    assert(!report.has_errors());

    heartstead::game::GameRuntimeConfig config;
    heartstead::scripting::ScriptHostApiDesc custom_api;
    custom_api.api_id = "magic.ward_anchor";
    custom_api.stage = heartstead::scripting::ScriptStage::runtime_server;
    custom_api.required_permissions = {heartstead::scripting::ScriptPermission::emit_commands};
    custom_api.arguments = {
        heartstead::scripting::ScriptHostApiArgument{
            "owner", heartstead::scripting::ScriptValueKind::string, false},
        heartstead::scripting::ScriptHostApiArgument{
            "prototype", heartstead::scripting::ScriptValueKind::string, false},
    };
    config.additional_script_host_apis.push_back(custom_api);
    heartstead::game::ScriptHostCommandRoute custom_route;
    custom_route.api_id = "magic.ward_anchor";
    custom_route.command_type = "process.start";
    custom_route.required_argument_keys = {"owner", "prototype"};
    config.additional_script_host_command_routes.push_back(custom_route);

    auto runtime = heartstead::game::GameRuntime::initialize(std::move(config), report);
    assert(runtime);
    assert(runtime.value().startup_report().script_host_api_count ==
           heartstead::game::GameRuntime::default_script_host_apis().size() + 1);
    assert(heartstead::scripting::find_script_host_api(
               runtime.value().startup_report().script_host_apis, "magic.ward_anchor") != nullptr);
    assert(runtime.value().startup_report().script_host_command_route_count ==
           heartstead::game::GameRuntime::default_script_host_command_routes().size() + 1);
    assert(runtime.value().make_script_host_command_router().find_route("magic.ward_anchor") !=
           nullptr);
}

void test_game_runtime_rejects_missing_base_mod() {
    heartstead::modding::ModValidationReport report;

    auto runtime =
        heartstead::game::GameRuntime::initialize(heartstead::game::GameRuntimeConfig{}, report);
    assert(!runtime);
    assert(runtime.error().code == "game_runtime.missing_base_mod");
}

void test_game_runtime_rejects_invalid_aggregate_content() {
    heartstead::content::ContentValidationReport report;
    report.diagnostics.push_back(heartstead::modding::ModDiagnostic{
        heartstead::modding::DiagnosticSeverity::error,
        {},
        "test.invalid_content",
        "intentional aggregate content error",
    });

    auto runtime =
        heartstead::game::GameRuntime::initialize(heartstead::game::GameRuntimeConfig{}, report);
    assert(!runtime);
    assert(runtime.error().code == "game_runtime.invalid_content");
}

void test_game_runtime_rejects_missing_required_kind() {
    const auto report = heartstead::modding::ModValidation::validate(source_root() / "mods");
    assert(!report.has_errors());

    heartstead::game::GameRuntimeConfig config;
    config.required_prototype_kinds.push_back("crop");

    auto runtime = heartstead::game::GameRuntime::initialize(std::move(config), report);
    assert(!runtime);
    assert(runtime.error().code == "game_runtime.missing_required_prototype_kind");
}

void test_game_runtime_rejects_invalid_script_host_api_registry() {
    const auto report = heartstead::modding::ModValidation::validate(source_root() / "mods");
    assert(!report.has_errors());

    heartstead::game::GameRuntimeConfig config;
    auto default_apis = heartstead::game::GameRuntime::default_script_host_apis();
    config.additional_script_host_apis.push_back(default_apis.front());

    auto runtime = heartstead::game::GameRuntime::initialize(std::move(config), report);
    assert(!runtime);
    assert(runtime.error().code == "game_runtime.invalid_script_host_api_registry");
}

void test_game_runtime_rejects_invalid_script_host_command_routes() {
    const auto report = heartstead::modding::ModValidation::validate(source_root() / "mods");
    assert(!report.has_errors());

    heartstead::game::GameRuntimeConfig duplicate_config;
    auto default_routes = heartstead::game::GameRuntime::default_script_host_command_routes();
    duplicate_config.additional_script_host_command_routes.push_back(default_routes.front());

    auto duplicate_runtime =
        heartstead::game::GameRuntime::initialize(std::move(duplicate_config), report);
    assert(!duplicate_runtime);
    assert(duplicate_runtime.error().code == "game_runtime.invalid_script_host_command_routes");

    heartstead::game::GameRuntimeConfig missing_api_config;
    heartstead::game::ScriptHostCommandRoute missing_api_route;
    missing_api_route.api_id = "magic.missing_api";
    missing_api_route.command_type = "process.start";
    missing_api_route.required_argument_keys = {"owner", "prototype"};
    missing_api_config.additional_script_host_command_routes.push_back(missing_api_route);

    auto missing_api_runtime =
        heartstead::game::GameRuntime::initialize(std::move(missing_api_config), report);
    assert(!missing_api_runtime);
    assert(missing_api_runtime.error().code == "game_runtime.script_host_route_api_missing");
}

void test_game_runtime_rejects_missing_selected_scenario() {
    const auto report = heartstead::modding::ModValidation::validate(source_root() / "mods");
    assert(!report.has_errors());

    heartstead::game::GameRuntimeConfig config;
    config.selected_scenario_id = "base:scenarios/missing";

    auto runtime = heartstead::game::GameRuntime::initialize(std::move(config), report);
    assert(!runtime);
    assert(runtime.error().code == "game_runtime.missing_selected_scenario");
}

void test_game_runtime_rejects_non_scenario_selection() {
    const auto report = heartstead::modding::ModValidation::validate(source_root() / "mods");
    assert(!report.has_errors());

    heartstead::game::GameRuntimeConfig config;
    config.selected_scenario_id = "base:items/raw_clay";

    auto runtime = heartstead::game::GameRuntime::initialize(std::move(config), report);
    assert(!runtime);
    assert(runtime.error().code == "game_runtime.invalid_selected_scenario_kind");
}

void test_game_runtime_rejects_missing_selected_scenario_definition() {
    auto report = heartstead::content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    report.scenario_definitions.clear();

    auto runtime =
        heartstead::game::GameRuntime::initialize(heartstead::game::GameRuntimeConfig{}, report);
    assert(!runtime);
    assert(runtime.error().code == "game_runtime.missing_selected_scenario_definition");
}

} // namespace

int main() {
    test_game_runtime_starts_from_base_mod();
    test_game_runtime_starts_from_aggregate_content();
    test_game_runtime_accepts_additional_script_host_api();
    test_game_runtime_rejects_missing_base_mod();
    test_game_runtime_rejects_invalid_aggregate_content();
    test_game_runtime_rejects_missing_required_kind();
    test_game_runtime_rejects_invalid_script_host_api_registry();
    test_game_runtime_rejects_invalid_script_host_command_routes();
    test_game_runtime_rejects_missing_selected_scenario();
    test_game_runtime_rejects_non_scenario_selection();
    test_game_runtime_rejects_missing_selected_scenario_definition();
    return 0;
}
