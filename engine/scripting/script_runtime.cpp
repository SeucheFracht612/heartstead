#include "engine/scripting/script_runtime.hpp"

#include "engine/core/ids.hpp"
#include "engine/scripting/luau/luau_backend.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace heartstead::scripting {

namespace {

[[nodiscard]] bool is_valid_script_function_name(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }

    for (const auto character : value) {
        const auto valid =
            (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' || character == '.';
        if (!valid) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool has_duplicate_permissions(const std::vector<ScriptPermission>& permissions) {
    for (std::size_t index = 0; index < permissions.size(); ++index) {
        for (std::size_t compare_index = index + 1; compare_index < permissions.size();
             ++compare_index) {
            if (permissions[index] == permissions[compare_index]) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] ScriptModuleInfo make_module_info(const ScriptModuleDesc& desc) {
    return ScriptModuleInfo{desc.module_id,   desc.source_mod_id, desc.source_path, desc.stage,
                            desc.api_version, desc.source.size(), desc.permissions};
}

[[nodiscard]] bool has_duplicate_host_apis(const std::vector<ScriptHostApiDesc>& host_apis) {
    for (std::size_t index = 0; index < host_apis.size(); ++index) {
        for (std::size_t compare_index = index + 1; compare_index < host_apis.size();
             ++compare_index) {
            if (host_apis[index].api_id == host_apis[compare_index].api_id) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] bool has_duplicate_host_api_arguments(const ScriptHostApiDesc& host_api) {
    for (std::size_t index = 0; index < host_api.arguments.size(); ++index) {
        for (std::size_t compare_index = index + 1; compare_index < host_api.arguments.size();
             ++compare_index) {
            if (host_api.arguments[index].name == host_api.arguments[compare_index].name) {
                return true;
            }
        }
    }
    return false;
}

class DisabledScriptRuntime final : public IScriptRuntime {
  public:
    explicit DisabledScriptRuntime(ScriptRuntimeDesc desc) : desc_(std::move(desc)) {}

    [[nodiscard]] ScriptBackend backend() const noexcept override {
        return ScriptBackend::disabled;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return script_backend_name(ScriptBackend::disabled);
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
        std::ranges::sort(result);
        return result;
    }

    [[nodiscard]] ScriptRuntimeStats stats() const noexcept override {
        ScriptRuntimeStats result;
        result.module_count = static_cast<std::uint32_t>(modules_.size());
        return result;
    }

    [[nodiscard]] const ScriptModuleInfo*
    find_module(std::string_view module_id) const noexcept override {
        const auto found = modules_.find(std::string(module_id));
        return found == modules_.end() ? nullptr : &found->second;
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

        modules_.emplace(desc.module_id, make_module_info(desc));
        return core::Status::ok();
    }

    [[nodiscard]] core::Status unload_module(std::string_view module_id) override {
        if (!core::PrototypeId::parse(module_id)) {
            return core::Status::failure("scripting.invalid_module_id",
                                         "script module id must be namespace:local_id");
        }
        if (modules_.erase(std::string(module_id)) == 0) {
            return core::Status::failure("scripting.module_not_loaded",
                                         "script module is not loaded");
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<ScriptCallResult> call(ScriptCallDesc desc) override {
        auto status = validate_script_call_desc(desc, desc_);
        if (!status) {
            return core::Result<ScriptCallResult>::failure(status.error().code,
                                                           status.error().message);
        }
        const auto* module = find_module(desc.module_id);
        if (module == nullptr) {
            return core::Result<ScriptCallResult>::failure("scripting.module_not_loaded",
                                                           "script module is not loaded");
        }
        auto permissions_status =
            validate_script_call_permissions(*module, desc.required_permissions);
        if (!permissions_status) {
            return core::Result<ScriptCallResult>::failure(permissions_status.error().code,
                                                           permissions_status.error().message);
        }
        return core::Result<ScriptCallResult>::failure(
            "scripting.runtime_disabled", "script execution is disabled in this backend");
    }

  private:
    ScriptRuntimeDesc desc_;
    std::unordered_map<std::string, ScriptModuleInfo> modules_;
};

} // namespace

ScriptValue ScriptValue::nil() noexcept {
    return ScriptValue{};
}

ScriptValue ScriptValue::boolean(bool value) noexcept {
    ScriptValue result;
    result.kind = ScriptValueKind::boolean;
    result.boolean_value = value;
    return result;
}

ScriptValue ScriptValue::number(double value) noexcept {
    ScriptValue result;
    result.kind = ScriptValueKind::number;
    result.number_value = value;
    return result;
}

ScriptValue ScriptValue::string(std::string value) {
    ScriptValue result;
    result.kind = ScriptValueKind::string;
    result.string_value = std::move(value);
    return result;
}

core::Result<std::unique_ptr<IScriptRuntime>> create_script_runtime(ScriptRuntimeDesc desc) {
    auto status = validate_script_runtime_desc(desc);
    if (!status) {
        return core::Result<std::unique_ptr<IScriptRuntime>>::failure(status.error().code,
                                                                      status.error().message);
    }

    switch (desc.backend) {
    case ScriptBackend::disabled:
        return core::Result<std::unique_ptr<IScriptRuntime>>::success(
            std::make_unique<DisabledScriptRuntime>(std::move(desc)));
    case ScriptBackend::luau:
        return luau::create_runtime(std::move(desc));
    }

    return core::Result<std::unique_ptr<IScriptRuntime>>::failure("scripting.unknown_backend",
                                                                  "unknown script backend");
}

core::Status validate_script_runtime_desc(const ScriptRuntimeDesc& desc) {
    if (desc.max_source_bytes == 0) {
        return core::Status::failure("scripting.invalid_source_limit",
                                     "script max source bytes must be non-zero");
    }
    if (desc.max_modules == 0) {
        return core::Status::failure("scripting.invalid_module_limit",
                                     "script max module count must be non-zero");
    }
    if (desc.max_call_arguments == 0) {
        return core::Status::failure("scripting.invalid_argument_limit",
                                     "script max call argument count must be non-zero");
    }
    if (desc.max_string_value_bytes == 0) {
        return core::Status::failure("scripting.invalid_string_limit",
                                     "script max string value bytes must be non-zero");
    }
    if (desc.max_vm_memory_bytes < 64u * 1024u) {
        return core::Status::failure("scripting.invalid_memory_limit",
                                     "script VM memory limit must be at least 64 KiB");
    }
    if (desc.max_call_wall_time_ms == 0 || desc.max_call_wall_time_ms > 60'000) {
        return core::Status::failure(
            "scripting.invalid_deadline_limit",
            "script call wall-time limit must be between 1 ms and 60 seconds");
    }
    if (desc.max_stack_depth == 0 || desc.max_stack_depth > 8'000) {
        return core::Status::failure("scripting.invalid_stack_limit",
                                     "script stack-depth limit must be between 1 and 8,000");
    }
    if (desc.max_emitted_events_per_call == 0 || desc.max_error_bytes == 0) {
        return core::Status::failure("scripting.invalid_output_limit",
                                     "script event and error-output limits must be non-zero");
    }
    return validate_script_host_api_registry(desc.host_apis);
}

core::Status validate_script_module_desc(const ScriptModuleDesc& desc,
                                         std::uint32_t max_source_bytes) {
    if (!core::PrototypeId::parse(desc.module_id)) {
        return core::Status::failure("scripting.invalid_module_id",
                                     "script module id must be namespace:local_id");
    }
    if (!core::is_valid_namespace_id(desc.source_mod_id)) {
        return core::Status::failure("scripting.invalid_source_mod_id",
                                     "script source mod id must be a valid namespace id");
    }
    if (desc.api_version == 0) {
        return core::Status::failure("scripting.invalid_api_version",
                                     "script module api version must be non-zero");
    }
    if (desc.source.empty()) {
        return core::Status::failure("scripting.empty_source", "script module source is required");
    }
    if (desc.source.size() > max_source_bytes) {
        return core::Status::failure("scripting.source_too_large",
                                     "script module source exceeds runtime source limit");
    }
    if (has_duplicate_permissions(desc.permissions)) {
        return core::Status::failure("scripting.duplicate_permission",
                                     "script module declares a permission more than once");
    }
    return core::Status::ok();
}

core::Status validate_script_call_desc(const ScriptCallDesc& desc) {
    if (!core::PrototypeId::parse(desc.module_id)) {
        return core::Status::failure("scripting.invalid_module_id",
                                     "script module id must be namespace:local_id");
    }
    if (!is_valid_script_function_name(desc.function_name)) {
        return core::Status::failure(
            "scripting.invalid_function_name",
            "script function name must contain letters, digits, underscores, or dots");
    }
    if (desc.instruction_budget == 0) {
        return core::Status::failure("scripting.invalid_instruction_budget",
                                     "script instruction budget must be non-zero");
    }
    if (has_duplicate_permissions(desc.required_permissions)) {
        return core::Status::failure("scripting.duplicate_permission",
                                     "script call requires a permission more than once");
    }
    for (const auto& argument : desc.arguments) {
        if (argument.kind == ScriptValueKind::number && !std::isfinite(argument.number_value)) {
            return core::Status::failure("scripting.invalid_number_argument",
                                         "script number arguments must be finite");
        }
    }
    return core::Status::ok();
}

core::Status validate_script_call_desc(const ScriptCallDesc& desc,
                                       const ScriptRuntimeDesc& runtime_desc) {
    auto status = validate_script_runtime_desc(runtime_desc);
    if (!status) {
        return status;
    }
    status = validate_script_call_desc(desc);
    if (!status) {
        return status;
    }
    if (desc.arguments.size() > runtime_desc.max_call_arguments) {
        return core::Status::failure("scripting.too_many_arguments",
                                     "script call exceeds runtime argument limit");
    }
    for (const auto& argument : desc.arguments) {
        status = validate_script_value(argument, runtime_desc.max_string_value_bytes);
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

core::Status validate_script_value(const ScriptValue& value, std::uint32_t max_string_value_bytes) {
    if (value.kind == ScriptValueKind::number && !std::isfinite(value.number_value)) {
        return core::Status::failure("scripting.invalid_number_argument",
                                     "script number arguments must be finite");
    }
    if (value.kind == ScriptValueKind::string &&
        value.string_value.size() > max_string_value_bytes) {
        return core::Status::failure("scripting.string_argument_too_large",
                                     "script string argument exceeds runtime byte limit");
    }
    return core::Status::ok();
}

core::Status validate_script_return_value(const ScriptValue& value,
                                          std::uint32_t max_string_value_bytes) {
    if (value.kind == ScriptValueKind::number && !std::isfinite(value.number_value)) {
        return core::Status::failure("scripting.invalid_number_return",
                                     "script number return values must be finite");
    }
    if (value.kind == ScriptValueKind::string &&
        value.string_value.size() > max_string_value_bytes) {
        return core::Status::failure("scripting.string_return_too_large",
                                     "script string return value exceeds runtime byte limit");
    }
    return core::Status::ok();
}

core::Status validate_script_call_result(const ScriptModuleInfo& module,
                                         const ScriptCallResult& result,
                                         const ScriptRuntimeDesc& runtime_desc) {
    auto status = validate_script_runtime_desc(runtime_desc);
    if (!status) {
        return status;
    }
    status = validate_script_return_value(result.return_value, runtime_desc.max_string_value_bytes);
    if (!status) {
        return status;
    }
    if (result.emitted_events.size() > runtime_desc.max_emitted_events_per_call) {
        return core::Status::failure("scripting.emitted_event_limit_exceeded",
                                     "script call emitted more events than the runtime permits");
    }
    return validate_script_emitted_events(module, result.emitted_events, runtime_desc);
}

core::Status
validate_script_call_permissions(const ScriptModuleInfo& module,
                                 const std::vector<ScriptPermission>& required_permissions) {
    for (const auto permission : required_permissions) {
        if (!script_module_has_permission(module, permission)) {
            return core::Status::failure("scripting.permission_denied",
                                         "script module lacks required permission: " +
                                             std::string(script_permission_name(permission)));
        }
    }
    return core::Status::ok();
}

bool script_module_has_permission(const ScriptModuleInfo& module,
                                  ScriptPermission permission) noexcept {
    for (const auto declared_permission : module.permissions) {
        if (declared_permission == permission) {
            return true;
        }
    }
    return false;
}

bool is_valid_script_host_api_id(std::string_view value) noexcept {
    if (value.empty() || value.front() == '.' || value.back() == '.') {
        return false;
    }

    bool previous_was_dot = false;
    for (const auto character : value) {
        if (character == '.') {
            if (previous_was_dot) {
                return false;
            }
            previous_was_dot = true;
            continue;
        }

        previous_was_dot = false;
        const auto valid = (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') || character == '_' ||
                           character == '-';
        if (!valid) {
            return false;
        }
    }
    return true;
}

bool is_valid_script_host_argument_name(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }

    for (const auto character : value) {
        const auto valid = (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') || character == '_';
        if (!valid) {
            return false;
        }
    }
    return true;
}

const ScriptHostApiDesc* find_script_host_api(const std::vector<ScriptHostApiDesc>& host_apis,
                                              std::string_view api_id) noexcept {
    for (const auto& host_api : host_apis) {
        if (host_api.api_id == api_id) {
            return &host_api;
        }
    }
    return nullptr;
}

core::Status validate_script_host_api_desc(const ScriptHostApiDesc& desc) {
    if (!is_valid_script_host_api_id(desc.api_id)) {
        return core::Status::failure(
            "scripting.invalid_host_api_id",
            "script host api id must be a lowercase dotted id without empty segments");
    }
    if (desc.min_module_api_version == 0) {
        return core::Status::failure("scripting.invalid_host_api_version",
                                     "script host api minimum module api version must be non-zero");
    }
    if (has_duplicate_permissions(desc.required_permissions)) {
        return core::Status::failure("scripting.duplicate_permission",
                                     "script host api requires a permission more than once");
    }
    if (has_duplicate_host_api_arguments(desc)) {
        return core::Status::failure("scripting.duplicate_host_api_argument",
                                     "script host api declares an argument more than once");
    }
    bool optional_seen = false;
    for (const auto& argument : desc.arguments) {
        if (!is_valid_script_host_argument_name(argument.name)) {
            return core::Status::failure(
                "scripting.invalid_host_api_argument",
                "script host api argument names must contain lowercase letters, digits, or "
                "underscores");
        }
        if (argument.kind == ScriptValueKind::nil) {
            return core::Status::failure("scripting.invalid_host_api_argument_kind",
                                         "script host api arguments cannot require nil values");
        }
        if (argument.optional) {
            optional_seen = true;
        } else if (optional_seen) {
            return core::Status::failure(
                "scripting.required_host_api_argument_after_optional",
                "script host api required arguments must precede optional arguments");
        }
    }
    return core::Status::ok();
}

core::Status validate_script_host_api_registry(const std::vector<ScriptHostApiDesc>& host_apis) {
    if (has_duplicate_host_apis(host_apis)) {
        return core::Status::failure("scripting.duplicate_host_api",
                                     "script runtime registers the same host api more than once");
    }

    for (const auto& host_api : host_apis) {
        auto status = validate_script_host_api_desc(host_api);
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

core::Status validate_script_host_api_call(const ScriptModuleInfo& module,
                                           const ScriptHostApiDesc& host_api) {
    if (module.stage != host_api.stage) {
        return core::Status::failure("scripting.host_api_stage_mismatch",
                                     "script emitted event stage does not match host api stage");
    }
    if (module.api_version < host_api.min_module_api_version) {
        return core::Status::failure(
            "scripting.host_api_version_unsupported",
            "script module api version is lower than the host api minimum");
    }
    return validate_script_call_permissions(module, host_api.required_permissions);
}

core::Status validate_script_emitted_events(const ScriptModuleInfo& module,
                                            const std::vector<ScriptEmittedEvent>& emitted_events,
                                            const ScriptRuntimeDesc& runtime_desc) {
    auto status = validate_script_runtime_desc(runtime_desc);
    if (!status) {
        return status;
    }

    for (const auto& emitted_event : emitted_events) {
        if (!is_valid_script_host_api_id(emitted_event.api_id)) {
            return core::Status::failure(
                "scripting.invalid_host_api_id",
                "script emitted event must be a lowercase dotted host api id");
        }

        const auto* host_api = find_script_host_api(runtime_desc.host_apis, emitted_event.api_id);
        if (host_api == nullptr) {
            return core::Status::failure("scripting.host_api_not_registered",
                                         "script emitted event is not registered as a host api: " +
                                             emitted_event.api_id);
        }

        status = validate_script_host_api_call(module, *host_api);
        if (!status) {
            return status;
        }

        std::size_t required_argument_count = 0;
        for (const auto& argument : host_api->arguments) {
            if (!argument.optional) {
                ++required_argument_count;
            }
        }
        if (emitted_event.arguments.size() < required_argument_count ||
            emitted_event.arguments.size() > host_api->arguments.size()) {
            return core::Status::failure(
                "scripting.host_api_argument_count_mismatch",
                "script emitted event argument count does not match host api descriptor");
        }

        for (std::size_t index = 0; index < emitted_event.arguments.size(); ++index) {
            const auto& argument = emitted_event.arguments[index];
            status = validate_script_value(argument, runtime_desc.max_string_value_bytes);
            if (!status) {
                return status;
            }
            if (argument.kind != host_api->arguments[index].kind) {
                return core::Status::failure(
                    "scripting.host_api_argument_type_mismatch",
                    "script emitted event argument type does not match host api descriptor");
            }
        }
    }
    return core::Status::ok();
}

ScriptBackendInfo script_backend_info(ScriptBackend backend) noexcept {
    switch (backend) {
    case ScriptBackend::disabled:
        return ScriptBackendInfo{
            ScriptBackend::disabled,
            script_backend_name(ScriptBackend::disabled),
            true,
            "available",
        };
    case ScriptBackend::luau:
        return luau::backend_info();
    }
    return ScriptBackendInfo{backend, "unknown", false, "unknown scripting backend"};
}

std::string_view script_backend_name(ScriptBackend backend) noexcept {
    switch (backend) {
    case ScriptBackend::disabled:
        return "disabled";
    case ScriptBackend::luau:
        return "luau";
    }
    return "unknown";
}

std::string_view script_stage_name(ScriptStage stage) noexcept {
    switch (stage) {
    case ScriptStage::runtime_server:
        return "runtime_server";
    case ScriptStage::runtime_client:
        return "runtime_client";
    case ScriptStage::migration:
        return "migration";
    }
    return "unknown";
}

std::string_view script_permission_name(ScriptPermission permission) noexcept {
    switch (permission) {
    case ScriptPermission::read_prototypes:
        return "read_prototypes";
    case ScriptPermission::read_assets:
        return "read_assets";
    case ScriptPermission::emit_commands:
        return "emit_commands";
    case ScriptPermission::read_save:
        return "read_save";
    case ScriptPermission::write_mod_state:
        return "write_mod_state";
    case ScriptPermission::client_ui:
        return "client_ui";
    }
    return "unknown";
}

std::optional<ScriptPermission> script_permission_from_name(std::string_view name) noexcept {
    constexpr ScriptPermission permissions[]{
        ScriptPermission::read_prototypes, ScriptPermission::read_assets,
        ScriptPermission::emit_commands,   ScriptPermission::read_save,
        ScriptPermission::write_mod_state, ScriptPermission::client_ui,
    };

    for (const auto permission : permissions) {
        if (script_permission_name(permission) == name) {
            return permission;
        }
    }
    return std::nullopt;
}

std::string_view script_value_kind_name(ScriptValueKind kind) noexcept {
    switch (kind) {
    case ScriptValueKind::nil:
        return "nil";
    case ScriptValueKind::boolean:
        return "boolean";
    case ScriptValueKind::number:
        return "number";
    case ScriptValueKind::string:
        return "string";
    }
    return "unknown";
}

} // namespace heartstead::scripting
