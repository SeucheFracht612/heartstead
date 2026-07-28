#include "engine/scripting/luau/luau_backend.hpp"

#include "engine/core/hash.hpp"
#include "engine/core/ids.hpp"

#if HEARTSTEAD_HAS_LUAU
#include <Luau/Compiler.h>
#include <lua.h>
#include <lualib.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heartstead::scripting::luau {

namespace {

#if HEARTSTEAD_HAS_LUAU

using SteadyClock = std::chrono::steady_clock;

[[nodiscard]] ScriptModuleInfo make_module_info(const ScriptModuleDesc& desc) {
    return ScriptModuleInfo{desc.module_id,   desc.source_mod_id, desc.source_path, desc.stage,
                            desc.api_version, desc.source.size(), desc.permissions};
}

struct VmKey {
    std::string source_mod_id;
    ScriptStage stage = ScriptStage::runtime_server;

    [[nodiscard]] bool operator<(const VmKey& other) const noexcept {
        if (source_mod_id != other.source_mod_id) {
            return source_mod_id < other.source_mod_id;
        }
        return stage < other.stage;
    }
};

struct VmMemoryContext {
    std::uint64_t limit_bytes = 0;
    std::uint64_t current_bytes = 0;
    std::uint64_t peak_bytes = 0;
};

void* limited_allocator(void* userdata, void* pointer, std::size_t old_size,
                        std::size_t new_size) noexcept {
    auto& memory = *static_cast<VmMemoryContext*>(userdata);
    const auto accounted_old = std::min(memory.current_bytes, static_cast<std::uint64_t>(old_size));
    const auto base = memory.current_bytes - accounted_old;

    if (new_size == 0) {
        std::free(pointer);
        memory.current_bytes = base;
        return nullptr;
    }

    const auto requested = static_cast<std::uint64_t>(new_size);
    if (requested > memory.limit_bytes || base > memory.limit_bytes - requested) {
        return nullptr;
    }

    void* resized = std::realloc(pointer, new_size);
    if (resized == nullptr) {
        return nullptr;
    }
    memory.current_bytes = base + requested;
    memory.peak_bytes = std::max(memory.peak_bytes, memory.current_bytes);
    return resized;
}

struct VmRecord {
    VmKey key;
    VmMemoryContext memory;
    lua_State* state = nullptr;

    ~VmRecord() {
        if (state != nullptr) {
            lua_close(state);
        }
    }

    VmRecord(const VmRecord&) = delete;
    VmRecord& operator=(const VmRecord&) = delete;
    VmRecord(VmRecord&&) = delete;
    VmRecord& operator=(VmRecord&&) = delete;

    explicit VmRecord(VmKey value) : key(std::move(value)) {}
};

struct ActiveCall {
    const ScriptRuntimeDesc* runtime_desc = nullptr;
    std::uint32_t instruction_budget = 0;
    std::uint64_t interrupt_count = 0;
    SteadyClock::time_point deadline;
    const std::atomic_bool* cancellation = nullptr;
    bool allow_emission = false;
    std::vector<ScriptEmittedEvent> emitted_events;
    std::optional<core::Error> failure;
};

struct ModuleRecord {
    ScriptModuleInfo info;
    VmKey vm_key;
    lua_State* thread = nullptr;
    int thread_reference = LUA_NOREF;
    int exports_reference = LUA_NOREF;
    std::size_t bytecode_bytes = 0;
    std::string source_fingerprint;
    ActiveCall* active_call = nullptr;
};

[[nodiscard]] std::string bounded_text(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string(text);
    }
    if (limit <= 3) {
        return std::string(text.substr(0, limit));
    }
    auto result = std::string(text.substr(0, limit - 3));
    result += "...";
    return result;
}

[[nodiscard]] std::string lua_error_text(lua_State* state, std::size_t limit) {
    std::size_t size = 0;
    const auto* text = lua_tolstring(state, -1, &size);
    if (text == nullptr) {
        return "Luau raised a non-string error";
    }
    return bounded_text(std::string_view(text, size), limit);
}

[[noreturn]] void interrupt_with_error(lua_State* state, ActiveCall& call, std::string code,
                                       std::string message) {
    call.failure = core::Error{std::move(code), std::move(message)};
    luaL_error(state, "Heartstead terminated the script call");
}

void execution_interrupt(lua_State* state, int) {
    auto* module = static_cast<ModuleRecord*>(lua_getthreaddata(state));
    if (module == nullptr || module->active_call == nullptr) {
        return;
    }
    auto& call = *module->active_call;
    ++call.interrupt_count;

    if (call.cancellation != nullptr && call.cancellation->load(std::memory_order_relaxed)) {
        interrupt_with_error(state, call, "scripting.call_cancelled",
                             "script call was cancelled by the host");
    }
    if (call.interrupt_count >= call.instruction_budget) {
        interrupt_with_error(state, call, "scripting.instruction_budget_exceeded",
                             "script call exceeded its VM interrupt budget");
    }
    if (static_cast<std::uint32_t>(lua_stackdepth(state)) > call.runtime_desc->max_stack_depth) {
        interrupt_with_error(state, call, "scripting.stack_limit_exceeded",
                             "script call exceeded its recursion/stack-depth limit");
    }
    if (SteadyClock::now() >= call.deadline) {
        interrupt_with_error(state, call, "scripting.deadline_exceeded",
                             "script call exceeded its wall-time deadline");
    }
}

void propagate_thread_data(lua_State* parent, lua_State* thread) {
    if (parent != nullptr) {
        lua_setthreaddata(thread, lua_getthreaddata(parent));
    }
}

[[nodiscard]] core::Result<ScriptValue> read_script_value(lua_State* state, int index,
                                                          std::uint32_t max_string_bytes,
                                                          std::string_view context) {
    switch (lua_type(state, index)) {
    case LUA_TNIL:
        return core::Result<ScriptValue>::success(ScriptValue::nil());
    case LUA_TBOOLEAN:
        return core::Result<ScriptValue>::success(
            ScriptValue::boolean(lua_toboolean(state, index) != 0));
    case LUA_TNUMBER: {
        const auto value = lua_tonumber(state, index);
        if (!std::isfinite(value)) {
            return core::Result<ScriptValue>::failure("scripting.invalid_number_" +
                                                          std::string(context),
                                                      "script number values must be finite");
        }
        return core::Result<ScriptValue>::success(ScriptValue::number(value));
    }
    case LUA_TSTRING: {
        std::size_t size = 0;
        const auto* value = lua_tolstring(state, index, &size);
        if (value == nullptr || size > max_string_bytes) {
            return core::Result<ScriptValue>::failure(
                "scripting.string_" + std::string(context) + "_too_large",
                "script string value exceeds the configured boundary limit");
        }
        return core::Result<ScriptValue>::success(ScriptValue::string(std::string(value, size)));
    }
    default:
        return core::Result<ScriptValue>::failure(
            "scripting.unsupported_" + std::string(context) + "_type",
            "script boundary values must be nil, boolean, number, or string");
    }
}

void push_script_value(lua_State* state, const ScriptValue& value) {
    switch (value.kind) {
    case ScriptValueKind::nil:
        lua_pushnil(state);
        break;
    case ScriptValueKind::boolean:
        lua_pushboolean(state, value.boolean_value ? 1 : 0);
        break;
    case ScriptValueKind::number:
        lua_pushnumber(state, value.number_value);
        break;
    case ScriptValueKind::string:
        lua_pushlstring(state, value.string_value.data(), value.string_value.size());
        break;
    }
}

[[noreturn]] void fail_host_call(lua_State* state, ActiveCall& call, const core::Error& error) {
    call.failure = error;
    luaL_error(state, "Heartstead rejected the script host call");
}

int emit_host_event(lua_State* state) {
    auto* module = static_cast<ModuleRecord*>(lua_getthreaddata(state));
    if (module == nullptr || module->active_call == nullptr) {
        luaL_error(state, "Heartstead host API called outside an active script call");
    }
    auto& call = *module->active_call;
    if (!call.allow_emission) {
        interrupt_with_error(state, call, "scripting.host_api_outside_call",
                             "host APIs cannot be called during module initialization");
    }

    const auto argument_count = lua_gettop(state);
    if (argument_count < 1 || lua_type(state, 1) != LUA_TSTRING) {
        interrupt_with_error(state, call, "scripting.invalid_host_api_id",
                             "emit requires a string host API id as its first argument");
    }
    if (static_cast<std::uint32_t>(argument_count - 1) > call.runtime_desc->max_call_arguments) {
        interrupt_with_error(state, call, "scripting.too_many_arguments",
                             "emitted host call exceeds the argument limit");
    }
    if (call.emitted_events.size() >= call.runtime_desc->max_emitted_events_per_call) {
        interrupt_with_error(state, call, "scripting.emitted_event_limit_exceeded",
                             "script call exceeded its emitted-event limit");
    }

    std::size_t api_id_size = 0;
    const auto* api_id = lua_tolstring(state, 1, &api_id_size);
    ScriptEmittedEvent event;
    event.api_id.assign(api_id, api_id_size);
    event.arguments.reserve(static_cast<std::size_t>(argument_count - 1));
    for (int index = 2; index <= argument_count; ++index) {
        auto value =
            read_script_value(state, index, call.runtime_desc->max_string_value_bytes, "argument");
        if (!value) {
            fail_host_call(state, call, value.error());
        }
        event.arguments.push_back(std::move(value).value());
    }

    const std::vector<ScriptEmittedEvent> validation_input{event};
    auto validation =
        validate_script_emitted_events(module->info, validation_input, *call.runtime_desc);
    if (!validation) {
        fail_host_call(state, call, validation.error());
    }
    call.emitted_events.push_back(std::move(event));
    lua_pushnil(state);
    return 1;
}

void clear_global(lua_State* state, const char* name) {
    lua_pushnil(state);
    lua_setglobal(state, name);
}

[[nodiscard]] core::Result<std::unique_ptr<VmRecord>> create_vm(VmKey key,
                                                                const ScriptRuntimeDesc& desc) {
    auto vm = std::make_unique<VmRecord>(std::move(key));
    vm->memory.limit_bytes = desc.max_vm_memory_bytes;
    try {
        vm->state = lua_newstate(limited_allocator, &vm->memory);
        if (vm->state == nullptr) {
            return core::Result<std::unique_ptr<VmRecord>>::failure(
                "scripting.vm_creation_failed",
                "Luau could not allocate a VM within the configured memory limit");
        }
        luaL_openlibs(vm->state);
        clear_global(vm->state, "debug");
        clear_global(vm->state, "getfenv");
        clear_global(vm->state, "setfenv");
        clear_global(vm->state, "loadstring");
        clear_global(vm->state, "newproxy");
        clear_global(vm->state, "collectgarbage");
        clear_global(vm->state, "os");
        clear_global(vm->state, "require");
        luaL_sandbox(vm->state);
        lua_callbacks(vm->state)->interrupt = execution_interrupt;
        lua_callbacks(vm->state)->userthread = propagate_thread_data;
    } catch (const std::bad_alloc&) {
        return core::Result<std::unique_ptr<VmRecord>>::failure(
            "scripting.memory_limit_exceeded", "Luau VM initialization exceeded its memory limit");
    } catch (const std::exception& error) {
        return core::Result<std::unique_ptr<VmRecord>>::failure(
            "scripting.vm_creation_failed", bounded_text(error.what(), desc.max_error_bytes));
    } catch (...) {
        return core::Result<std::unique_ptr<VmRecord>>::failure(
            "scripting.vm_creation_failed", "Luau VM initialization failed with an unknown error");
    }
    return core::Result<std::unique_ptr<VmRecord>>::success(std::move(vm));
}

class StackReset final {
  public:
    explicit StackReset(lua_State* state) : state_(state) {}
    ~StackReset() {
        lua_settop(state_, 0);
    }

    StackReset(const StackReset&) = delete;
    StackReset& operator=(const StackReset&) = delete;

  private:
    lua_State* state_;
};

[[nodiscard]] core::Error execution_error(lua_State* state, int status,
                                          const ScriptRuntimeDesc& desc, const ActiveCall& call,
                                          std::string fallback_code) {
    if (call.failure.has_value()) {
        return *call.failure;
    }
    if (status == LUA_ERRMEM) {
        return core::Error{"scripting.memory_limit_exceeded",
                           "script execution exceeded the VM memory limit"};
    }
    return core::Error{std::move(fallback_code), lua_error_text(state, desc.max_error_bytes)};
}

[[nodiscard]] bool resolve_export(lua_State* state, int exports_reference,
                                  std::string_view function_name) {
    lua_getref(state, exports_reference);
    std::size_t segment_start = 0;
    while (segment_start <= function_name.size()) {
        const auto segment_end = function_name.find('.', segment_start);
        const auto segment = segment_end == std::string_view::npos
                                 ? function_name.substr(segment_start)
                                 : function_name.substr(segment_start, segment_end - segment_start);
        if (lua_type(state, -1) != LUA_TTABLE) {
            return false;
        }
        const auto segment_text = std::string(segment);
        lua_getfield(state, -1, segment_text.c_str());
        lua_remove(state, -2);
        if (segment_end == std::string_view::npos) {
            break;
        }
        segment_start = segment_end + 1;
    }
    return lua_type(state, -1) == LUA_TFUNCTION;
}

class LuauRuntime final : public IScriptRuntime {
  public:
    explicit LuauRuntime(ScriptRuntimeDesc desc) : desc_(std::move(desc)) {
        stats_.memory_limit_bytes_per_vm = desc_.max_vm_memory_bytes;
    }

    [[nodiscard]] ScriptBackend backend() const noexcept override {
        return ScriptBackend::luau;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return script_backend_name(ScriptBackend::luau);
    }

    [[nodiscard]] std::size_t module_count() const noexcept override {
        return modules_.size();
    }

    [[nodiscard]] std::vector<std::string> module_ids() const override {
        std::vector<std::string> result;
        result.reserve(modules_.size());
        for (const auto& [module_id, _] : modules_) {
            result.push_back(module_id);
        }
        return result;
    }

    [[nodiscard]] ScriptRuntimeStats stats() const noexcept override {
        auto result = stats_;
        result.vm_count = static_cast<std::uint32_t>(vms_.size());
        result.module_count = static_cast<std::uint32_t>(modules_.size());
        result.current_memory_bytes = 0;
        result.peak_memory_bytes = 0;
        for (const auto& [_, vm] : vms_) {
            result.current_memory_bytes += vm->memory.current_bytes;
            result.peak_memory_bytes += vm->memory.peak_bytes;
        }
        return result;
    }

    [[nodiscard]] const ScriptModuleInfo*
    find_module(std::string_view module_id) const noexcept override {
        const auto found = modules_.find(std::string(module_id));
        return found == modules_.end() ? nullptr : &found->second->info;
    }

    [[nodiscard]] core::Status load_module(ScriptModuleDesc desc) override {
        auto status = validate_script_module_desc(desc, desc_.max_source_bytes);
        if (!status) {
            return status;
        }
        if (modules_.contains(desc.module_id)) {
            return core::Status::failure("scripting.duplicate_module",
                                         "script module is already loaded");
        }
        if (modules_.size() >= desc_.max_modules) {
            return core::Status::failure("scripting.module_limit_reached",
                                         "script runtime module limit has been reached");
        }

        std::string bytecode;
        try {
            Luau::CompileOptions options;
            options.optimizationLevel = 1;
            options.debugLevel = 1;
            const char* mutable_globals[] = {"emit", nullptr};
            options.mutableGlobals = mutable_globals;
            bytecode = Luau::compile(desc.source, options);
        } catch (const std::bad_alloc&) {
            return core::Status::failure(
                "scripting.compiler_memory_exhausted",
                "Luau compiler exhausted host memory while compiling the module");
        } catch (const std::exception& error) {
            return core::Status::failure("scripting.luau_compile_error",
                                         bounded_text(error.what(), desc_.max_error_bytes));
        }

        const VmKey key{desc.source_mod_id, desc.stage};
        auto vm_found = vms_.find(key);
        bool created_vm = false;
        if (vm_found == vms_.end()) {
            auto created = create_vm(key, desc_);
            if (!created) {
                return core::Status::failure(created.error().code, created.error().message);
            }
            vm_found = vms_.emplace(key, std::move(created).value()).first;
            created_vm = true;
        }
        auto& vm = *vm_found->second;

        auto module = std::make_unique<ModuleRecord>();
        module->info = make_module_info(desc);
        module->vm_key = key;
        module->bytecode_bytes = bytecode.size();
        module->source_fingerprint = core::stable_hash64_hex(desc.source);

        const auto cleanup_failure = [&]() {
            if (module->exports_reference != LUA_NOREF) {
                lua_unref(module->thread, module->exports_reference);
                module->exports_reference = LUA_NOREF;
            }
            if (module->thread_reference != LUA_NOREF) {
                lua_unref(vm.state, module->thread_reference);
                module->thread_reference = LUA_NOREF;
            }
            module->thread = nullptr;
            (void)lua_gc(vm.state, LUA_GCCOLLECT, 0);
            if (created_vm) {
                vms_.erase(key);
            }
        };

        try {
            module->thread = lua_newthread(vm.state);
            module->thread_reference = lua_ref(vm.state, -1);
            lua_setthreaddata(module->thread, module.get());
            luaL_sandboxthread(module->thread);
            lua_pushcfunction(module->thread, emit_host_event, "emit");
            lua_setglobal(module->thread, "emit");

            const auto loaded = luau_load(module->thread, desc.module_id.c_str(), bytecode.data(),
                                          bytecode.size(), 0);
            if (loaded != LUA_OK) {
                const auto message = lua_error_text(module->thread, desc_.max_error_bytes);
                lua_settop(module->thread, 0);
                cleanup_failure();
                return core::Status::failure("scripting.luau_compile_error", message);
            }

            ActiveCall initialization;
            initialization.runtime_desc = &desc_;
            initialization.instruction_budget = 100'000;
            initialization.deadline =
                SteadyClock::now() + std::chrono::milliseconds(desc_.max_call_wall_time_ms);
            module->active_call = &initialization;
            const auto initialized = lua_pcall(module->thread, 0, 1, 0);
            module->active_call = nullptr;
            stats_.interrupt_count += initialization.interrupt_count;
            if (initialized != LUA_OK) {
                const auto error =
                    execution_error(module->thread, initialized, desc_, initialization,
                                    "scripting.module_initialization_failed");
                lua_settop(module->thread, 0);
                cleanup_failure();
                return core::Status::failure(error.code, error.message);
            }
            if (lua_type(module->thread, -1) != LUA_TTABLE) {
                lua_settop(module->thread, 0);
                cleanup_failure();
                return core::Status::failure(
                    "scripting.luau_no_exports",
                    "script module must return a table of exported functions");
            }
            lua_setreadonly(module->thread, -1, 1);
            module->exports_reference = lua_ref(module->thread, -1);
        } catch (const std::bad_alloc&) {
            cleanup_failure();
            return core::Status::failure("scripting.memory_limit_exceeded",
                                         "module initialization exceeded its VM memory limit");
        } catch (const std::exception& error) {
            cleanup_failure();
            return core::Status::failure("scripting.module_initialization_failed",
                                         bounded_text(error.what(), desc_.max_error_bytes));
        } catch (...) {
            cleanup_failure();
            return core::Status::failure("scripting.module_initialization_failed",
                                         "module initialization failed with an unknown error");
        }

        stats_.compiled_source_bytes += desc.source.size();
        stats_.compiled_bytecode_bytes += bytecode.size();
        modules_.emplace(desc.module_id, std::move(module));
        return core::Status::ok();
    }

    [[nodiscard]] core::Status unload_module(std::string_view module_id) override {
        if (!core::PrototypeId::parse(module_id)) {
            return core::Status::failure("scripting.invalid_module_id",
                                         "script module id must be namespace:local_id");
        }
        const auto found = modules_.find(std::string(module_id));
        if (found == modules_.end()) {
            return core::Status::failure("scripting.module_not_loaded",
                                         "script module is not loaded");
        }
        const auto vm_key = found->second->vm_key;
        auto& module = *found->second;
        const auto vm = vms_.find(vm_key);
        if (module.exports_reference != LUA_NOREF) {
            lua_unref(module.thread, module.exports_reference);
        }
        if (vm != vms_.end() && module.thread_reference != LUA_NOREF) {
            lua_unref(vm->second->state, module.thread_reference);
        }
        modules_.erase(found);

        const auto vm_still_used = std::ranges::any_of(modules_, [&vm_key](const auto& entry) {
            return entry.second->vm_key.source_mod_id == vm_key.source_mod_id &&
                   entry.second->vm_key.stage == vm_key.stage;
        });
        if (!vm_still_used) {
            vms_.erase(vm_key);
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<ScriptCallResult> call(ScriptCallDesc desc) override {
        auto status = validate_script_call_desc(desc, desc_);
        if (!status) {
            return core::Result<ScriptCallResult>::failure(status.error().code,
                                                           status.error().message);
        }

        const auto found = modules_.find(desc.module_id);
        if (found == modules_.end()) {
            return core::Result<ScriptCallResult>::failure("scripting.module_not_loaded",
                                                           "script module is not loaded");
        }
        auto& module = *found->second;
        if (module.info.stage != desc.stage) {
            return core::Result<ScriptCallResult>::failure(
                "scripting.stage_mismatch", "script call stage does not match loaded module stage");
        }
        auto permission_status =
            validate_script_call_permissions(module.info, desc.required_permissions);
        if (!permission_status) {
            return core::Result<ScriptCallResult>::failure(permission_status.error().code,
                                                           permission_status.error().message);
        }

        StackReset stack(module.thread);
        if (!resolve_export(module.thread, module.exports_reference, desc.function_name)) {
            return core::Result<ScriptCallResult>::failure(
                "scripting.function_not_found",
                "script module does not export the requested function");
        }

        lua_Debug function_info{};
        if (lua_getinfo(module.thread, -1, "a", &function_info) == 0) {
            return core::Result<ScriptCallResult>::failure(
                "scripting.function_introspection_failed",
                "Luau could not inspect the requested export");
        }
        const auto minimum_arguments = static_cast<std::size_t>(function_info.nparams);
        if ((!function_info.isvararg && desc.arguments.size() != minimum_arguments) ||
            (function_info.isvararg && desc.arguments.size() < minimum_arguments)) {
            return core::Result<ScriptCallResult>::failure(
                "scripting.argument_count_mismatch",
                "script function call does not match exported parameter count");
        }
        for (const auto& argument : desc.arguments) {
            push_script_value(module.thread, argument);
        }

        ActiveCall active;
        active.runtime_desc = &desc_;
        active.instruction_budget = desc.instruction_budget;
        active.deadline =
            SteadyClock::now() + std::chrono::milliseconds(desc_.max_call_wall_time_ms);
        active.cancellation = desc.cancellation;
        active.allow_emission = true;
        module.active_call = &active;

        const auto started = SteadyClock::now();
        const auto call_status =
            lua_pcall(module.thread, static_cast<int>(desc.arguments.size()), 1, 0);
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - started);
        module.active_call = nullptr;

        ++stats_.call_count;
        stats_.interrupt_count += active.interrupt_count;
        stats_.last_call_microseconds =
            static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed.count()));
        stats_.total_call_microseconds += stats_.last_call_microseconds;
        if (call_status != LUA_OK) {
            ++stats_.failed_call_count;
            const auto error = execution_error(module.thread, call_status, desc_, active,
                                               "scripting.runtime_error");
            // Drop the error object and any abandoned call frames before the
            // full collection. In particular, this lets a VM recover from a
            // script that reached its allocation limit.
            lua_resetthread(module.thread);
            const auto vm = vms_.find(module.vm_key);
            if (vm != vms_.end()) {
                (void)lua_gc(vm->second->state, LUA_GCCOLLECT, 0);
            }
            return core::Result<ScriptCallResult>::failure(error.code, error.message);
        }

        auto return_value =
            read_script_value(module.thread, -1, desc_.max_string_value_bytes, "return");
        if (!return_value) {
            ++stats_.failed_call_count;
            return core::Result<ScriptCallResult>::failure(return_value.error().code,
                                                           return_value.error().message);
        }
        ScriptCallResult result;
        result.return_value = std::move(return_value).value();
        result.emitted_events = std::move(active.emitted_events);
        result.consumed_instruction_estimate = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(std::max<std::uint64_t>(1, active.interrupt_count),
                                    std::numeric_limits<std::uint32_t>::max()));

        status = validate_script_call_result(module.info, result, desc_);
        if (!status) {
            ++stats_.failed_call_count;
            return core::Result<ScriptCallResult>::failure(status.error().code,
                                                           status.error().message);
        }
        stats_.emitted_event_count += result.emitted_events.size();
        return core::Result<ScriptCallResult>::success(std::move(result));
    }

  private:
    ScriptRuntimeDesc desc_;
    std::map<VmKey, std::unique_ptr<VmRecord>> vms_;
    std::map<std::string, std::unique_ptr<ModuleRecord>> modules_;
    ScriptRuntimeStats stats_;
};

#endif

} // namespace

ScriptBackendInfo backend_info() noexcept {
#if HEARTSTEAD_HAS_LUAU
    return ScriptBackendInfo{
        ScriptBackend::luau,
        script_backend_name(ScriptBackend::luau),
        true,
        "Luau 0.729 compiler/VM backend with isolated sandboxed environments",
    };
#else
    return ScriptBackendInfo{
        ScriptBackend::luau,
        script_backend_name(ScriptBackend::luau),
        false,
        "Luau backend was disabled at build time",
    };
#endif
}

core::Result<std::unique_ptr<IScriptRuntime>> create_runtime(ScriptRuntimeDesc desc) {
#if HEARTSTEAD_HAS_LUAU
    return core::Result<std::unique_ptr<IScriptRuntime>>::success(
        std::make_unique<LuauRuntime>(std::move(desc)));
#else
    (void)desc;
    return core::Result<std::unique_ptr<IScriptRuntime>>::failure(
        "scripting.backend_unavailable", "Luau backend was disabled at build time");
#endif
}

} // namespace heartstead::scripting::luau
