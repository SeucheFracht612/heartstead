#include "engine/debug/inspection.hpp"
#include "engine/scripting/script_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace heartstead;

[[nodiscard]] scripting::ScriptModuleDesc
module(std::string id, std::string mod_id, scripting::ScriptStage stage, std::string source,
       std::vector<scripting::ScriptPermission> permissions = {}) {
    scripting::ScriptModuleDesc result;
    result.module_id = std::move(id);
    result.source_mod_id = std::move(mod_id);
    result.source_path = "tests/neutral_conformance/" + result.module_id + ".luau";
    result.source = std::move(source);
    result.stage = stage;
    result.permissions = std::move(permissions);
    return result;
}

[[nodiscard]] scripting::ScriptHostApiDesc host_api(std::string id, scripting::ScriptStage stage,
                                                    scripting::ScriptPermission permission,
                                                    bool with_string_argument = false) {
    scripting::ScriptHostApiDesc result;
    result.api_id = std::move(id);
    result.stage = stage;
    result.required_permissions = {permission};
    if (with_string_argument) {
        result.arguments.push_back({"value", scripting::ScriptValueKind::string, false});
    }
    return result;
}

[[nodiscard]] scripting::ScriptCallDesc call(std::string module_id, std::string function,
                                             scripting::ScriptStage stage,
                                             std::vector<scripting::ScriptValue> arguments = {},
                                             std::uint32_t budget = 100'000) {
    scripting::ScriptCallDesc result;
    result.module_id = std::move(module_id);
    result.function_name = std::move(function);
    result.stage = stage;
    result.arguments = std::move(arguments);
    result.instruction_budget = budget;
    return result;
}

void test_neutral_stage_conformance_and_isolation() {
    using namespace scripting;

    ScriptRuntimeDesc desc{ScriptBackend::luau};
    desc.host_apis = {
        host_api("neutral.server", ScriptStage::runtime_server, ScriptPermission::emit_commands,
                 true),
        host_api("neutral.client", ScriptStage::runtime_client, ScriptPermission::client_ui, true),
        host_api("neutral.migration", ScriptStage::migration, ScriptPermission::write_mod_state,
                 true),
    };
    auto runtime = create_script_runtime(desc);
    assert(runtime);

    const auto server_a =
        module("alpha:scripts/runtime_server/a", "alpha", ScriptStage::runtime_server,
               "shared_marker = \"private-alpha\"\n"
               "local count = 0\n"
               "return {\n"
               "  read = function() return shared_marker end,\n"
               "  next = function() count += 1 return count end,\n"
               "  emit_value = function(value) return emit(\"neutral.server\", value) end\n"
               "}",
               {ScriptPermission::emit_commands});
    const auto server_b =
        module("alpha:scripts/runtime_server/b", "alpha", ScriptStage::runtime_server,
               "return { read = function() return shared_marker end }");
    const auto other_mod =
        module("beta:scripts/runtime_server/a", "beta", ScriptStage::runtime_server,
               "return { read = function() return shared_marker end }");
    const auto client =
        module("alpha:scripts/runtime_client/a", "alpha", ScriptStage::runtime_client,
               "return { emit_value = function(value) return emit(\"neutral.client\", value) end }",
               {ScriptPermission::client_ui});
    const auto migration = module(
        "alpha:migrations/0001_neutral", "alpha", ScriptStage::migration,
        "return { emit_value = function(value) return emit(\"neutral.migration\", value) end }",
        {ScriptPermission::write_mod_state});

    // Deliberately load out of lexical order; the public enumeration remains deterministic.
    assert(runtime.value()->load_module(other_mod));
    assert(runtime.value()->load_module(server_b));
    assert(runtime.value()->load_module(server_a));
    assert(runtime.value()->load_module(migration));
    assert(runtime.value()->load_module(client));
    const auto ids = runtime.value()->module_ids();
    assert(std::ranges::is_sorted(ids));

    auto private_value =
        runtime.value()->call(call(server_a.module_id, "read", ScriptStage::runtime_server));
    assert(private_value);
    assert(private_value.value().return_value.string_value == "private-alpha");
    auto same_vm_isolated =
        runtime.value()->call(call(server_b.module_id, "read", ScriptStage::runtime_server));
    assert(same_vm_isolated);
    assert(same_vm_isolated.value().return_value.kind == ScriptValueKind::nil);
    auto other_vm_isolated =
        runtime.value()->call(call(other_mod.module_id, "read", ScriptStage::runtime_server));
    assert(other_vm_isolated);
    assert(other_vm_isolated.value().return_value.kind == ScriptValueKind::nil);

    auto first =
        runtime.value()->call(call(server_a.module_id, "next", ScriptStage::runtime_server));
    auto second =
        runtime.value()->call(call(server_a.module_id, "next", ScriptStage::runtime_server));
    assert(first && second);
    assert(first.value().return_value.number_value == 1.0);
    assert(second.value().return_value.number_value == 2.0);

    const auto exercise_stage = [&runtime](const ScriptModuleDesc& value, ScriptStage stage,
                                           std::string expected_api) {
        auto invoked = runtime.value()->call(
            call(value.module_id, "emit_value", stage, {ScriptValue::string("neutral")}));
        assert(invoked);
        assert(invoked.value().return_value.kind == ScriptValueKind::nil);
        assert(invoked.value().emitted_events.size() == 1);
        assert(invoked.value().emitted_events.front().api_id == expected_api);
    };
    exercise_stage(server_a, ScriptStage::runtime_server, "neutral.server");
    exercise_stage(client, ScriptStage::runtime_client, "neutral.client");
    exercise_stage(migration, ScriptStage::migration, "neutral.migration");

    auto wrong_stage =
        runtime.value()->call(call(client.module_id, "emit_value", ScriptStage::runtime_server,
                                   {ScriptValue::string("neutral")}));
    assert(!wrong_stage);
    assert(wrong_stage.error().code == "scripting.stage_mismatch");

    auto before_reload = runtime.value()->stats();
    assert(before_reload.vm_count == 4);
    assert(before_reload.module_count == 5);
    assert(before_reload.memory_limit_bytes_per_vm == 8u * 1024u * 1024u);
    assert(before_reload.compiled_source_bytes > 0);
    assert(before_reload.compiled_bytecode_bytes > 0);
    assert(before_reload.emitted_event_count == 3);
    assert(before_reload.current_memory_bytes <=
           before_reload.vm_count * before_reload.memory_limit_bytes_per_vm);

    const auto inspected = debug::Inspector::inspect(before_reload);
    assert(inspected.object_type == "script_runtime_stats");
    assert(inspected.find_field("interrupt_count") != nullptr);
    assert(inspected.find_field("peak_memory_bytes") != nullptr);

    assert(runtime.value()->unload_module(server_a.module_id));
    assert(runtime.value()->load_module(server_a));
    auto reset =
        runtime.value()->call(call(server_a.module_id, "next", ScriptStage::runtime_server));
    assert(reset);
    assert(reset.value().return_value.number_value == 1.0);
}

void test_hostile_sources_fail_closed_and_runtime_recovers() {
    using namespace scripting;

    ScriptRuntimeDesc desc{ScriptBackend::luau};
    // Leave enough headroom above Luau's standard-library baseline to prove
    // both allocation-limit enforcement and post-failure VM recovery.
    desc.max_vm_memory_bytes = 1024u * 1024u;
    desc.max_stack_depth = 16;
    desc.max_string_value_bytes = 32;
    desc.max_emitted_events_per_call = 2;
    desc.max_error_bytes = 64;
    desc.host_apis = {
        host_api("neutral.ping", ScriptStage::runtime_server, ScriptPermission::emit_commands),
    };
    auto runtime = create_script_runtime(desc);
    assert(runtime);

    const auto hostile =
        module("hostile:scripts/runtime_server/probes", "hostile", ScriptStage::runtime_server,
               "local function recurse() return recurse() end\n"
               "return {\n"
               "  globals_closed = function()\n"
               "    return io == nil and os == nil and debug == nil and package == nil\n"
               "      and require == nil and socket == nil and loadfile == nil\n"
               "      and dofile == nil and loadstring == nil and getfenv == nil\n"
               "      and setfenv == nil\n"
               "  end,\n"
               "  spin = function() while true do end end,\n"
               "  yield_now = function() coroutine.yield() end,\n"
               "  coroutine_spin = function()\n"
               "    local worker = coroutine.create(function() while true do end end)\n"
               "    coroutine.resume(worker)\n"
               "  end,\n"
               "  coroutine_emit = function()\n"
               "    local worker = coroutine.create(function() emit(\"neutral.ping\") end)\n"
               "    local ok = coroutine.resume(worker)\n"
               "    return ok\n"
               "  end,\n"
               "  recurse = recurse,\n"
               "  memory_bomb = function()\n"
               "    local values = {}\n"
               "    while true do values[#values + 1] = string.rep(\"x\", 1024) end\n"
               "  end,\n"
               "  oversized_return = function() return string.rep(\"x\", 33) end,\n"
               "  too_many_events = function()\n"
               "    emit(\"neutral.ping\") emit(\"neutral.ping\") emit(\"neutral.ping\")\n"
               "  end,\n"
               "  explode = function() error(string.rep(\"x\", 512)) end,\n"
               "  echo = function(value) return value end\n"
               "}",
               {ScriptPermission::emit_commands});
    assert(runtime.value()->load_module(hostile));

    auto globals = runtime.value()->call(
        call(hostile.module_id, "globals_closed", ScriptStage::runtime_server));
    assert(globals);
    assert(globals.value().return_value.kind == ScriptValueKind::boolean);
    assert(globals.value().return_value.boolean_value);

    auto infinite = runtime.value()->call(
        call(hostile.module_id, "spin", ScriptStage::runtime_server, {}, 100));
    assert(!infinite);
    assert(infinite.error().code == "scripting.instruction_budget_exceeded");

    auto yielded =
        runtime.value()->call(call(hostile.module_id, "yield_now", ScriptStage::runtime_server));
    assert(!yielded);
    assert(yielded.error().code == "scripting.runtime_error");

    auto coroutine_infinite = runtime.value()->call(
        call(hostile.module_id, "coroutine_spin", ScriptStage::runtime_server, {}, 100));
    assert(!coroutine_infinite);
    assert(coroutine_infinite.error().code == "scripting.instruction_budget_exceeded");

    auto coroutine_event = runtime.value()->call(
        call(hostile.module_id, "coroutine_emit", ScriptStage::runtime_server));
    assert(coroutine_event);
    assert(coroutine_event.value().return_value.boolean_value);
    assert(coroutine_event.value().emitted_events.size() == 1);

    auto recursion = runtime.value()->call(
        call(hostile.module_id, "recurse", ScriptStage::runtime_server, {}, 100'000));
    assert(!recursion);
    assert(recursion.error().code == "scripting.stack_limit_exceeded");

    auto memory = runtime.value()->call(
        call(hostile.module_id, "memory_bomb", ScriptStage::runtime_server, {}, 100'000));
    assert(!memory);
    assert(memory.error().code == "scripting.memory_limit_exceeded");

    auto oversized = runtime.value()->call(
        call(hostile.module_id, "oversized_return", ScriptStage::runtime_server));
    assert(!oversized);
    assert(oversized.error().code == "scripting.string_return_too_large");

    auto events = runtime.value()->call(
        call(hostile.module_id, "too_many_events", ScriptStage::runtime_server));
    assert(!events);
    assert(events.error().code == "scripting.emitted_event_limit_exceeded");

    auto bounded_error =
        runtime.value()->call(call(hostile.module_id, "explode", ScriptStage::runtime_server));
    assert(!bounded_error);
    assert(bounded_error.error().code == "scripting.runtime_error");
    assert(bounded_error.error().message.size() <= desc.max_error_bytes);

    std::atomic_bool cancelled{true};
    auto cancelled_call = call(hostile.module_id, "spin", ScriptStage::runtime_server, {},
                               std::numeric_limits<std::uint32_t>::max());
    cancelled_call.cancellation = &cancelled;
    auto cancellation = runtime.value()->call(cancelled_call);
    assert(!cancellation);
    assert(cancellation.error().code == "scripting.call_cancelled");

    // Every hostile failure is protected; the same VM remains usable.
    auto recovered = runtime.value()->call(
        call(hostile.module_id, "echo", ScriptStage::runtime_server, {ScriptValue::string("ok")}));
    assert(recovered);
    assert(recovered.value().return_value.string_value == "ok");

    auto malformed = hostile;
    malformed.module_id = "hostile:scripts/runtime_server/malformed";
    malformed.source = "return { broken = function(";
    auto malformed_result = runtime.value()->load_module(malformed);
    assert(!malformed_result);
    assert(malformed_result.error().code == "scripting.luau_compile_error");

    auto oversized_source = hostile;
    oversized_source.module_id = "hostile:scripts/runtime_server/oversized_source";
    oversized_source.source = std::string(desc.max_source_bytes + 1, 'x');
    auto oversized_source_result = runtime.value()->load_module(oversized_source);
    assert(!oversized_source_result);
    assert(oversized_source_result.error().code == "scripting.source_too_large");

    const auto stats = runtime.value()->stats();
    assert(stats.failed_call_count >= 7);
    assert(stats.interrupt_count > 0);
    assert(stats.current_memory_bytes <= stats.memory_limit_bytes_per_vm);
}

void test_deadline_interrupt() {
    using namespace scripting;

    ScriptRuntimeDesc desc{ScriptBackend::luau};
    desc.max_call_wall_time_ms = 1;
    auto runtime = create_script_runtime(desc);
    assert(runtime);
    const auto deadline =
        module("deadline:scripts/runtime_server/probe", "deadline", ScriptStage::runtime_server,
               "return { spin = function() while true do end end }");
    assert(runtime.value()->load_module(deadline));
    auto result =
        runtime.value()->call(call(deadline.module_id, "spin", ScriptStage::runtime_server, {},
                                   std::numeric_limits<std::uint32_t>::max()));
    assert(!result);
    assert(result.error().code == "scripting.deadline_exceeded");
}

} // namespace

int main() {
    const auto info =
        heartstead::scripting::script_backend_info(heartstead::scripting::ScriptBackend::luau);
    if (!info.available) {
        auto runtime = heartstead::scripting::create_script_runtime(
            heartstead::scripting::ScriptRuntimeDesc{heartstead::scripting::ScriptBackend::luau});
        assert(!runtime);
        assert(runtime.error().code == "scripting.backend_unavailable");
        return 0;
    }

    test_neutral_stage_conformance_and_isolation();
    test_hostile_sources_fail_closed_and_runtime_recovers();
    test_deadline_interrupt();
    return 0;
}
