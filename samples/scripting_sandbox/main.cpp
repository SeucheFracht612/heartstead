#include "engine/core/logging.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/scripting/script_host_event.hpp"
#include "engine/scripting/script_runtime.hpp"

#include <string>

int main(int argc, char** argv) {
    return heartstead::core::run_no_argument_process_entry(argc, argv, [] {
        using namespace heartstead;

        core::log(core::LogLevel::info, "Heartstead scripting sandbox starting");

        const auto luau_info = scripting::script_backend_info(scripting::ScriptBackend::luau);
        core::log(core::LogLevel::info, "Luau backend status: " + std::string(luau_info.status));
        if (!luau_info.available) {
            core::log(core::LogLevel::info,
                      "Luau backend is disabled; scripting boundary smoke test skipped");
            return 0;
        }

        scripting::ScriptHostApiDesc sandbox_ready_api;
        sandbox_ready_api.api_id = "sandbox.ready";
        sandbox_ready_api.stage = scripting::ScriptStage::runtime_server;
        sandbox_ready_api.required_permissions.push_back(
            scripting::ScriptPermission::emit_commands);

        scripting::ScriptRuntimeDesc runtime_desc{scripting::ScriptBackend::luau, 4096, 32, 8,
                                                  1024};
        runtime_desc.host_apis.push_back(sandbox_ready_api);
        core::log(core::LogLevel::info,
                  "Script limits: max modules " + std::to_string(runtime_desc.max_modules) +
                      ", max args " + std::to_string(runtime_desc.max_call_arguments) +
                      ", max string bytes " + std::to_string(runtime_desc.max_string_value_bytes));

        auto runtime = scripting::create_script_runtime(runtime_desc);
        if (!runtime) {
            core::log(core::LogLevel::error, runtime.error().message);
            return 1;
        }

        scripting::ScriptModuleDesc module;
        module.module_id = "base:scripts/runtime_server/debug_ping";
        module.source_mod_id = "base";
        module.source_path = "mods/base/scripts/runtime_server/debug_ping.luau";
        module.source = "return {"
                        " ping = function(value) return value end,"
                        " notify = function() return emit(\"sandbox.ready\") end"
                        "}";
        module.stage = scripting::ScriptStage::runtime_server;
        module.permissions.push_back(scripting::ScriptPermission::read_prototypes);
        module.permissions.push_back(scripting::ScriptPermission::emit_commands);

        auto loaded = runtime.value()->load_module(module);
        if (!loaded) {
            core::log(core::LogLevel::error, loaded.error().message);
            return 1;
        }

        const auto* info = runtime.value()->find_module(module.module_id);
        if (info == nullptr) {
            core::log(core::LogLevel::error, "script module was not loaded");
            return 1;
        }

        core::log(core::LogLevel::info, "Loaded " + info->module_id + " for stage " +
                                            std::string(scripting::script_stage_name(info->stage)));

        if (!scripting::script_module_has_permission(
                *info, scripting::ScriptPermission::read_prototypes)) {
            core::log(core::LogLevel::error,
                      "script module is missing expected read_prototypes grant");
            return 1;
        }
        if (!scripting::script_module_has_permission(*info,
                                                     scripting::ScriptPermission::emit_commands)) {
            core::log(core::LogLevel::error,
                      "script module is missing expected emit_commands grant");
            return 1;
        }

        auto call = runtime.value()->call(
            scripting::ScriptCallDesc{module.module_id,
                                      "ping",
                                      scripting::ScriptStage::runtime_server,
                                      {scripting::ScriptValue::string("hello from script sandbox")},
                                      100,
                                      {scripting::ScriptPermission::read_prototypes}});
        if (!call || call.value().return_value.kind != scripting::ScriptValueKind::string) {
            core::log(core::LogLevel::error,
                      call ? "script call returned unexpected value" : call.error().message);
            return 1;
        }

        core::log(core::LogLevel::info,
                  "Script call returned " + call.value().return_value.string_value + " through " +
                      std::string(runtime.value()->backend_name()) + " backend");

        auto event_call = runtime.value()->call(
            scripting::ScriptCallDesc{module.module_id,
                                      "notify",
                                      scripting::ScriptStage::runtime_server,
                                      {},
                                      100,
                                      {scripting::ScriptPermission::emit_commands}});
        if (!event_call || event_call.value().emitted_events.size() != 1) {
            core::log(core::LogLevel::error,
                      event_call ? "script event call returned unexpected event count"
                                 : event_call.error().message);
            return 1;
        }

        core::log(core::LogLevel::info,
                  "Script emitted event " + event_call.value().emitted_events.front().api_id +
                      " with " +
                      std::to_string(event_call.value().emitted_events.front().arguments.size()) +
                      " arguments");

        scripting::ScriptHostEventQueue host_events;
        auto event_batch = host_events.enqueue_from_call(
            *info,
            scripting::ScriptCallDesc{module.module_id,
                                      "notify",
                                      scripting::ScriptStage::runtime_server,
                                      {},
                                      100,
                                      {scripting::ScriptPermission::emit_commands}},
            event_call.value(), runtime_desc);
        if (!event_batch || event_batch.value().events.size() != 1) {
            core::log(core::LogLevel::error,
                      event_batch ? "script host event queue returned unexpected event count"
                                  : event_batch.error().message);
            return 1;
        }

        core::log(core::LogLevel::info,
                  "Queued script host event " + event_batch.value().events.front().api_id);
        return 0;
    });
}
