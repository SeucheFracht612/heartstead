#pragma once

#include "engine/core/result.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heartstead::scripting {

enum class ScriptBackend {
    disabled,
    luau,
};

enum class ScriptStage {
    runtime_server,
    runtime_client,
    migration,
};

enum class ScriptPermission {
    read_prototypes,
    read_assets,
    emit_commands,
    read_save,
    write_mod_state,
    client_ui,
};

enum class ScriptValueKind {
    nil,
    boolean,
    number,
    string,
};

struct ScriptBackendInfo {
    ScriptBackend backend = ScriptBackend::disabled;
    std::string_view name;
    bool available = false;
    std::string_view status;
};

struct ScriptValue {
    ScriptValueKind kind = ScriptValueKind::nil;
    bool boolean_value = false;
    double number_value = 0.0;
    std::string string_value;

    [[nodiscard]] static ScriptValue nil() noexcept;
    [[nodiscard]] static ScriptValue boolean(bool value) noexcept;
    [[nodiscard]] static ScriptValue number(double value) noexcept;
    [[nodiscard]] static ScriptValue string(std::string value);
};

struct ScriptModuleDesc {
    std::string module_id;
    std::string source_mod_id;
    std::filesystem::path source_path;
    std::string source;
    ScriptStage stage = ScriptStage::runtime_server;
    std::uint32_t api_version = 1;
    std::vector<ScriptPermission> permissions;
};

struct ScriptModuleInfo {
    std::string module_id;
    std::string source_mod_id;
    std::filesystem::path source_path;
    ScriptStage stage = ScriptStage::runtime_server;
    std::uint32_t api_version = 1;
    std::size_t source_bytes = 0;
    std::vector<ScriptPermission> permissions;
};

struct ScriptCallDesc {
    std::string module_id;
    std::string function_name;
    ScriptStage stage = ScriptStage::runtime_server;
    std::vector<ScriptValue> arguments;
    std::uint32_t instruction_budget = 100'000;
    std::vector<ScriptPermission> required_permissions;
    const std::atomic_bool* cancellation = nullptr;
};

struct ScriptHostApiArgument {
    std::string name;
    ScriptValueKind kind = ScriptValueKind::string;
    bool optional = false;
};

struct ScriptEmittedEvent {
    std::string api_id;
    std::vector<ScriptValue> arguments;
};

struct ScriptCallResult {
    ScriptValue return_value;
    std::uint32_t consumed_instruction_estimate = 0;
    std::vector<ScriptEmittedEvent> emitted_events;
};

struct ScriptHostApiDesc {
    std::string api_id;
    ScriptStage stage = ScriptStage::runtime_server;
    std::uint32_t min_module_api_version = 1;
    std::vector<ScriptPermission> required_permissions;
    std::vector<ScriptHostApiArgument> arguments;
};

struct ScriptRuntimeDesc {
    ScriptBackend backend = ScriptBackend::disabled;
    std::uint32_t max_source_bytes = 256u * 1024u;
    std::uint32_t max_modules = 256;
    std::uint32_t max_call_arguments = 32;
    std::uint32_t max_string_value_bytes = 64u * 1024u;
    std::uint64_t max_vm_memory_bytes = 8u * 1024u * 1024u;
    std::uint32_t max_call_wall_time_ms = 50;
    std::uint32_t max_stack_depth = 128;
    std::uint32_t max_emitted_events_per_call = 64;
    std::uint32_t max_error_bytes = 4u * 1024u;
    std::vector<ScriptHostApiDesc> host_apis;

    ScriptRuntimeDesc() = default;

    explicit ScriptRuntimeDesc(ScriptBackend backend_value,
                               std::uint32_t max_source_bytes_value = 256u * 1024u,
                               std::uint32_t max_modules_value = 256,
                               std::uint32_t max_call_arguments_value = 32,
                               std::uint32_t max_string_value_bytes_value = 64u * 1024u,
                               std::vector<ScriptHostApiDesc> host_apis_value = {})
        : backend(backend_value), max_source_bytes(max_source_bytes_value),
          max_modules(max_modules_value), max_call_arguments(max_call_arguments_value),
          max_string_value_bytes(max_string_value_bytes_value),
          host_apis(std::move(host_apis_value)) {}
};

struct ScriptRuntimeStats {
    std::uint32_t vm_count = 0;
    std::uint32_t module_count = 0;
    std::uint64_t current_memory_bytes = 0;
    std::uint64_t peak_memory_bytes = 0;
    std::uint64_t memory_limit_bytes_per_vm = 0;
    std::uint64_t compiled_source_bytes = 0;
    std::uint64_t compiled_bytecode_bytes = 0;
    std::uint64_t call_count = 0;
    std::uint64_t failed_call_count = 0;
    std::uint64_t interrupt_count = 0;
    std::uint64_t emitted_event_count = 0;
    std::uint64_t total_call_microseconds = 0;
    std::uint64_t last_call_microseconds = 0;
};

class IScriptRuntime {
  public:
    virtual ~IScriptRuntime() = default;

    [[nodiscard]] virtual ScriptBackend backend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual std::size_t module_count() const noexcept = 0;
    [[nodiscard]] virtual std::vector<std::string> module_ids() const = 0;
    [[nodiscard]] virtual ScriptRuntimeStats stats() const noexcept = 0;
    [[nodiscard]] virtual const ScriptModuleInfo*
    find_module(std::string_view module_id) const noexcept = 0;

    [[nodiscard]] virtual core::Status load_module(ScriptModuleDesc desc) = 0;
    [[nodiscard]] virtual core::Status unload_module(std::string_view module_id) = 0;
    [[nodiscard]] virtual core::Result<ScriptCallResult> call(ScriptCallDesc desc) = 0;
};

[[nodiscard]] core::Result<std::unique_ptr<IScriptRuntime>>
create_script_runtime(ScriptRuntimeDesc desc);

[[nodiscard]] core::Status validate_script_runtime_desc(const ScriptRuntimeDesc& desc);
[[nodiscard]] core::Status validate_script_module_desc(const ScriptModuleDesc& desc,
                                                       std::uint32_t max_source_bytes);
[[nodiscard]] core::Status validate_script_call_desc(const ScriptCallDesc& desc);
[[nodiscard]] core::Status validate_script_call_desc(const ScriptCallDesc& desc,
                                                     const ScriptRuntimeDesc& runtime_desc);
[[nodiscard]] core::Status validate_script_value(const ScriptValue& value,
                                                 std::uint32_t max_string_value_bytes);
[[nodiscard]] core::Status validate_script_return_value(const ScriptValue& value,
                                                        std::uint32_t max_string_value_bytes);
[[nodiscard]] core::Status validate_script_call_result(const ScriptModuleInfo& module,
                                                       const ScriptCallResult& result,
                                                       const ScriptRuntimeDesc& runtime_desc);
[[nodiscard]] core::Status
validate_script_call_permissions(const ScriptModuleInfo& module,
                                 const std::vector<ScriptPermission>& required_permissions);
[[nodiscard]] core::Status validate_script_host_api_desc(const ScriptHostApiDesc& desc);
[[nodiscard]] core::Status
validate_script_host_api_registry(const std::vector<ScriptHostApiDesc>& host_apis);
[[nodiscard]] core::Status validate_script_host_api_call(const ScriptModuleInfo& module,
                                                         const ScriptHostApiDesc& host_api);
[[nodiscard]] core::Status
validate_script_emitted_events(const ScriptModuleInfo& module,
                               const std::vector<ScriptEmittedEvent>& emitted_events,
                               const ScriptRuntimeDesc& runtime_desc);
[[nodiscard]] bool script_module_has_permission(const ScriptModuleInfo& module,
                                                ScriptPermission permission) noexcept;
[[nodiscard]] bool is_valid_script_host_api_id(std::string_view value) noexcept;
[[nodiscard]] bool is_valid_script_host_argument_name(std::string_view value) noexcept;
[[nodiscard]] const ScriptHostApiDesc*
find_script_host_api(const std::vector<ScriptHostApiDesc>& host_apis,
                     std::string_view api_id) noexcept;

[[nodiscard]] ScriptBackendInfo script_backend_info(ScriptBackend backend) noexcept;
[[nodiscard]] std::string_view script_backend_name(ScriptBackend backend) noexcept;
[[nodiscard]] std::string_view script_stage_name(ScriptStage stage) noexcept;
[[nodiscard]] std::string_view script_permission_name(ScriptPermission permission) noexcept;
[[nodiscard]] std::optional<ScriptPermission>
script_permission_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view script_value_kind_name(ScriptValueKind kind) noexcept;

} // namespace heartstead::scripting
