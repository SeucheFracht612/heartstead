#include "game/runtime/game_runtime.hpp"

#include "engine/content/content_validation.hpp"
#include "engine/core/ids.hpp"
#include "engine/modding/prototype_registry.hpp"
#include "engine/scenarios/scenario.hpp"
#include "game/runtime/game_inspection.hpp"

#include <algorithm>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] bool has_mod_id(const std::vector<modding::ModManifest>& mods,
                              std::string_view mod_id) noexcept {
    return std::ranges::any_of(
        mods, [mod_id](const modding::ModManifest& mod) { return mod.id == mod_id; });
}

[[nodiscard]] std::string missing_kind_message(std::string_view kind) {
    return "game runtime required prototype kind has no loaded prototypes: " + std::string(kind);
}

[[nodiscard]] scripting::ScriptHostApiDesc
make_script_host_api(std::string api_id, scripting::ScriptStage stage,
                     std::vector<scripting::ScriptPermission> permissions,
                     std::vector<scripting::ScriptHostApiArgument> arguments = {},
                     std::uint32_t min_module_api_version = 1) {
    scripting::ScriptHostApiDesc host_api;
    host_api.api_id = std::move(api_id);
    host_api.stage = stage;
    host_api.min_module_api_version = min_module_api_version;
    host_api.required_permissions = std::move(permissions);
    host_api.arguments = std::move(arguments);
    return host_api;
}

[[nodiscard]] scripting::ScriptHostApiArgument
make_script_host_api_argument(std::string name, scripting::ScriptValueKind kind,
                              bool optional = false) {
    scripting::ScriptHostApiArgument argument;
    argument.name = std::move(name);
    argument.kind = kind;
    argument.optional = optional;
    return argument;
}

[[nodiscard]] ScriptHostCommandRoute
make_script_host_command_route(std::string api_id, std::string command_type,
                               std::vector<std::string> required_argument_keys,
                               std::vector<std::string> optional_argument_keys = {}) {
    ScriptHostCommandRoute route;
    route.api_id = std::move(api_id);
    route.command_type = std::move(command_type);
    route.stage = scripting::ScriptStage::runtime_server;
    route.required_argument_keys = std::move(required_argument_keys);
    route.optional_argument_keys = std::move(optional_argument_keys);
    return route;
}

[[nodiscard]] core::Status populate_script_host_apis(GameRuntimeStartupReport& report,
                                                     const GameRuntimeConfig& config) {
    report.script_host_apis.clear();
    if (config.register_default_script_host_apis) {
        report.script_host_apis = GameRuntime::default_script_host_apis();
    }
    report.script_host_apis.insert(report.script_host_apis.end(),
                                   config.additional_script_host_apis.begin(),
                                   config.additional_script_host_apis.end());
    report.script_host_api_count = report.script_host_apis.size();

    auto status = scripting::validate_script_host_api_registry(report.script_host_apis);
    if (!status) {
        return core::Status::failure("game_runtime.invalid_script_host_api_registry",
                                     status.error().message);
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status populate_script_host_command_routes(GameRuntimeStartupReport& report,
                                                               const GameRuntimeConfig& config) {
    report.script_host_command_routes.clear();
    if (config.register_default_script_host_command_routes) {
        report.script_host_command_routes = GameRuntime::default_script_host_command_routes();
    }
    report.script_host_command_routes.insert(report.script_host_command_routes.end(),
                                             config.additional_script_host_command_routes.begin(),
                                             config.additional_script_host_command_routes.end());
    report.script_host_command_route_count = report.script_host_command_routes.size();

    auto status = validate_script_host_command_routes(report.script_host_command_routes);
    if (!status) {
        return core::Status::failure("game_runtime.invalid_script_host_command_routes",
                                     status.error().message);
    }

    for (const auto& route : report.script_host_command_routes) {
        const auto* host_api =
            scripting::find_script_host_api(report.script_host_apis, route.api_id);
        if (host_api == nullptr) {
            return core::Status::failure(
                "game_runtime.script_host_route_api_missing",
                "script host command route references an unregistered host API: " + route.api_id);
        }
        const auto route_argument_count =
            route.required_argument_keys.size() + route.optional_argument_keys.size();
        if (route_argument_count != host_api->arguments.size()) {
            return core::Status::failure(
                "game_runtime.script_host_route_argument_mismatch",
                "script host command route argument count does not match host API: " +
                    route.api_id);
        }
        std::size_t argument_index = 0;
        for (const auto& required_key : route.required_argument_keys) {
            if (host_api->arguments[argument_index].optional ||
                host_api->arguments[argument_index].name != required_key) {
                return core::Status::failure(
                    "game_runtime.script_host_route_argument_mismatch",
                    "script host command route required arguments do not match host API: " +
                        route.api_id);
            }
            ++argument_index;
        }
        for (const auto& optional_key : route.optional_argument_keys) {
            if (!host_api->arguments[argument_index].optional ||
                host_api->arguments[argument_index].name != optional_key) {
                return core::Status::failure(
                    "game_runtime.script_host_route_argument_mismatch",
                    "script host command route optional arguments do not match host API: " +
                        route.api_id);
            }
            ++argument_index;
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status populate_startup_report(
    GameRuntimeStartupReport& report, const GameRuntimeConfig& config,
    const std::vector<modding::ModManifest>& mods, const modding::PrototypeRegistry& registry,
    std::size_t resource_pack_count, std::size_t active_asset_count,
    std::size_t voxel_palette_entry_count, std::size_t material_definition_count,
    std::size_t material_asset_reference_count, bool aggregate_content_validated) {
    report.base_mod_id = config.base_mod_id;
    report.mod_count = mods.size();
    report.resource_pack_count = resource_pack_count;
    report.prototype_count = registry.size();
    report.active_asset_count = active_asset_count;
    report.voxel_palette_entry_count = voxel_palette_entry_count;
    report.material_definition_count = material_definition_count;
    report.material_asset_reference_count = material_asset_reference_count;
    report.aggregate_content_validated = aggregate_content_validated;
    report.base_mod_loaded = has_mod_id(mods, config.base_mod_id);
    report.systems = GameRuntime::default_systems();

    auto script_host_apis_status = populate_script_host_apis(report, config);
    if (!script_host_apis_status) {
        return script_host_apis_status;
    }
    auto script_host_routes_status = populate_script_host_command_routes(report, config);
    if (!script_host_routes_status) {
        return script_host_routes_status;
    }

    if (config.require_base_mod && !report.base_mod_loaded) {
        return core::Status::failure("game_runtime.missing_base_mod",
                                     "required base mod is not loaded: " + config.base_mod_id);
    }

    if (config.require_selected_scenario) {
        if (config.selected_scenario_id.empty()) {
            return core::Status::failure("game_runtime.invalid_selected_scenario",
                                         "selected scenario id must not be empty");
        }

        const auto selected_scenario = core::PrototypeId::parse(config.selected_scenario_id);
        if (!selected_scenario) {
            return core::Status::failure("game_runtime.invalid_selected_scenario",
                                         "selected scenario id is not a valid prototype id: " +
                                             config.selected_scenario_id);
        }

        const auto* scenario = registry.find(selected_scenario.value());
        if (scenario == nullptr) {
            return core::Status::failure("game_runtime.missing_selected_scenario",
                                         "selected scenario prototype is not loaded: " +
                                             selected_scenario.value().value());
        }
        if (scenario->kind != modding::PrototypeKinds::scenario) {
            return core::Status::failure("game_runtime.invalid_selected_scenario_kind",
                                         "selected scenario prototype is not kind scenario: " +
                                             selected_scenario.value().value());
        }
        report.selected_scenario_id = selected_scenario.value().value();
    }

    for (const auto& kind : config.required_prototype_kinds) {
        if (kind.empty()) {
            return core::Status::failure("game_runtime.invalid_required_kind",
                                         "game runtime required prototype kind must not be empty");
        }
        const auto count = registry.count_kind(kind);
        report.prototypes_by_kind.emplace(kind, count);
        if (count == 0) {
            return core::Status::failure("game_runtime.missing_required_prototype_kind",
                                         missing_kind_message(kind));
        }
    }

    report.initialized = true;
    return core::Status::ok();
}

[[nodiscard]] const scenarios::ScenarioDefinition*
find_scenario_definition(const std::vector<scenarios::ScenarioDefinition>& definitions,
                         const core::PrototypeId& scenario_id) noexcept {
    const auto found = std::ranges::find_if(
        definitions, [&scenario_id](const scenarios::ScenarioDefinition& definition) {
            return definition.prototype_id == scenario_id;
        });
    return found == definitions.end() ? nullptr : &*found;
}

} // namespace

GameRuntimeConfig::GameRuntimeConfig()
    : required_prototype_kinds(GameRuntime::default_required_prototype_kinds()) {}

GameRuntime::GameRuntime() = default;

GameRuntime::~GameRuntime() {
    (void)shutdown();
}

GameRuntime::GameRuntime(GameRuntime&&) noexcept = default;

GameRuntime& GameRuntime::operator=(GameRuntime&&) noexcept = default;

std::size_t GameRuntimeStartupReport::count_kind(std::string_view kind) const noexcept {
    const auto found = prototypes_by_kind.find(std::string(kind));
    return found == prototypes_by_kind.end() ? 0 : found->second;
}

std::vector<std::string> GameRuntime::default_required_prototype_kinds() {
    return {std::string(modding::PrototypeKinds::item),
            std::string(modding::PrototypeKinds::cargo),
            std::string(modding::PrototypeKinds::entity),
            std::string(modding::PrototypeKinds::voxel),
            std::string(modding::PrototypeKinds::build_piece),
            std::string(modding::PrototypeKinds::assembly),
            std::string(modding::PrototypeKinds::workpiece),
            std::string(modding::PrototypeKinds::process),
            std::string(modding::PrototypeKinds::room_descriptor),
            std::string(modding::PrototypeKinds::material),
            std::string(modding::PrototypeKinds::scenario)};
}

std::vector<GameSystemDescriptor> GameRuntime::default_systems() {
    return {{GameSystemKind::building, "building"},   {GameSystemKind::rooms, "rooms"},
            {GameSystemKind::crafting, "crafting"},   {GameSystemKind::production, "production"},
            {GameSystemKind::logistics, "logistics"}, {GameSystemKind::farming, "farming"},
            {GameSystemKind::animals, "animals"},     {GameSystemKind::ecology, "ecology"},
            {GameSystemKind::combat, "combat"},       {GameSystemKind::sleep, "sleep"},
            {GameSystemKind::magic, "magic"},         {GameSystemKind::workpieces, "workpieces"}};
}

std::vector<scripting::ScriptHostApiDesc> GameRuntime::default_script_host_apis() {
    using scripting::ScriptPermission;
    using scripting::ScriptStage;

    return {
        make_script_host_api(
            "world.set_voxel", ScriptStage::runtime_server, {ScriptPermission::emit_commands},
            {make_script_host_api_argument("chunk", scripting::ScriptValueKind::string),
             make_script_host_api_argument("voxel", scripting::ScriptValueKind::string),
             make_script_host_api_argument("prototype", scripting::ScriptValueKind::string)}),
        make_script_host_api(
            "inventory.transfer", ScriptStage::runtime_server, {ScriptPermission::emit_commands},
            {make_script_host_api_argument("source_owner", scripting::ScriptValueKind::string),
             make_script_host_api_argument("destination_owner", scripting::ScriptValueKind::string),
             make_script_host_api_argument("source_slot", scripting::ScriptValueKind::number),
             make_script_host_api_argument("destination_slot", scripting::ScriptValueKind::number),
             make_script_host_api_argument("count", scripting::ScriptValueKind::number)}),
        make_script_host_api(
            "workpiece.edit_cell", ScriptStage::runtime_server, {ScriptPermission::emit_commands},
            {make_script_host_api_argument("workpiece_id", scripting::ScriptValueKind::string),
             make_script_host_api_argument("operation", scripting::ScriptValueKind::string),
             make_script_host_api_argument("coord", scripting::ScriptValueKind::string),
             make_script_host_api_argument("cell", scripting::ScriptValueKind::string, true)}),
        make_script_host_api(
            "process.start", ScriptStage::runtime_server, {ScriptPermission::emit_commands},
            {make_script_host_api_argument("owner", scripting::ScriptValueKind::string),
             make_script_host_api_argument("prototype", scripting::ScriptValueKind::string)}),
        make_script_host_api(
            "ui.notify", ScriptStage::runtime_client, {ScriptPermission::client_ui},
            {make_script_host_api_argument("message", scripting::ScriptValueKind::string)}),
        make_script_host_api(
            "save.migrate_mod_state", ScriptStage::migration,
            {ScriptPermission::read_save, ScriptPermission::write_mod_state},
            {make_script_host_api_argument("mod_id", scripting::ScriptValueKind::string),
             make_script_host_api_argument("payload", scripting::ScriptValueKind::string)}),
    };
}

std::vector<ScriptHostCommandRoute> GameRuntime::default_script_host_command_routes() {
    return {
        make_script_host_command_route("world.set_voxel", "world.set_voxel",
                                       {"chunk", "voxel", "prototype"}),
        make_script_host_command_route(
            "inventory.transfer", "inventory.transfer_items",
            {"source_owner", "destination_owner", "source_slot", "destination_slot", "count"}),
        make_script_host_command_route("workpiece.edit_cell", "workpiece.edit_cell",
                                       {"workpiece_id", "operation", "coord"}, {"cell"}),
        make_script_host_command_route("process.start", "process.start", {"owner", "prototype"}),
    };
}

core::Result<GameRuntime>
GameRuntime::initialize(GameRuntimeConfig config,
                        const modding::ModValidationReport& content_report) {
    if (config.base_mod_id.empty()) {
        return core::Result<GameRuntime>::failure("game_runtime.invalid_base_mod",
                                                  "game runtime base mod id must not be empty");
    }
    if (content_report.has_errors()) {
        return core::Result<GameRuntime>::failure(
            "game_runtime.invalid_mod_content",
            "game runtime cannot start with mod validation errors");
    }

    GameRuntime runtime;
    runtime.config_ = std::move(config);
    runtime.prototypes_ = std::make_shared<modding::PrototypeRegistry>(content_report.registry);
    auto palette = world::voxel_palette_from_prototypes(content_report.registry);
    if (!palette) {
        return core::Result<GameRuntime>::failure(palette.error().code, palette.error().message);
    }
    runtime.voxel_palette_ = std::make_shared<world::VoxelPalette>(std::move(palette).value());
    runtime.assets_ = std::make_shared<assets::AssetCatalog>();
    runtime.sound_events_ = std::make_shared<audio::SoundEventRegistry>();
    auto startup_status =
        populate_startup_report(runtime.startup_report_, runtime.config_, content_report.mods,
                                content_report.registry, 0, 0, 0, 0, 0, false);
    if (!startup_status) {
        return core::Result<GameRuntime>::failure(startup_status.error().code,
                                                  startup_status.error().message);
    }

    return core::Result<GameRuntime>::success(std::move(runtime));
}

core::Result<GameRuntime>
GameRuntime::initialize(GameRuntimeConfig config,
                        const heartstead::content::ContentValidationReport& content_report) {
    if (config.base_mod_id.empty()) {
        return core::Result<GameRuntime>::failure("game_runtime.invalid_base_mod",
                                                  "game runtime base mod id must not be empty");
    }
    if (content_report.has_errors()) {
        return core::Result<GameRuntime>::failure(
            "game_runtime.invalid_content",
            "game runtime cannot start with aggregate content validation errors");
    }

    GameRuntime runtime;
    runtime.config_ = std::move(config);
    runtime.prototypes_ = std::make_shared<modding::PrototypeRegistry>(content_report.registry);
    runtime.voxel_palette_ = std::make_shared<world::VoxelPalette>(content_report.voxel_palette);
    runtime.assets_ = std::make_shared<assets::AssetCatalog>(content_report.asset_catalog);
    runtime.sound_events_ =
        std::make_shared<audio::SoundEventRegistry>(content_report.sound_events);
    runtime.startup_report_.sound_event_count = content_report.sound_events.size();
    auto startup_status = populate_startup_report(
        runtime.startup_report_, runtime.config_, content_report.mods, content_report.registry,
        content_report.resource_packs.size(), content_report.asset_catalog.active_count(),
        content_report.voxel_palette.size(), content_report.material_registry.size(),
        content_report.material_assets.references.size(), true);
    if (!startup_status) {
        return core::Result<GameRuntime>::failure(startup_status.error().code,
                                                  startup_status.error().message);
    }

    if (runtime.config_.require_selected_scenario) {
        const auto scenario_id = core::PrototypeId::parse(runtime.config_.selected_scenario_id);
        if (!scenario_id) {
            return core::Result<GameRuntime>::failure(
                "game_runtime.invalid_selected_scenario",
                "selected scenario id is not a valid prototype id: " +
                    runtime.config_.selected_scenario_id);
        }
        const auto* scenario =
            find_scenario_definition(content_report.scenario_definitions, scenario_id.value());
        if (scenario == nullptr) {
            return core::Result<GameRuntime>::failure(
                "game_runtime.missing_selected_scenario_definition",
                "selected scenario definition is not loaded: " + scenario_id.value().value());
        }
        runtime.startup_report_.selected_scenario_start_region = scenario->start_region;
        runtime.startup_report_.selected_scenario_spawn_mode =
            std::string(scenarios::scenario_spawn_mode_name(scenario->spawn_mode));
    }

    return core::Result<GameRuntime>::success(std::move(runtime));
}

bool GameRuntime::is_initialized() const noexcept {
    return startup_report_.initialized;
}

const GameRuntimeConfig& GameRuntime::config() const noexcept {
    return config_;
}

const GameRuntimeStartupReport& GameRuntime::startup_report() const noexcept {
    return startup_report_;
}

scripting::ScriptRuntimeDesc
GameRuntime::make_script_runtime_desc(scripting::ScriptBackend backend) const {
    scripting::ScriptRuntimeDesc desc{backend};
    desc.host_apis = startup_report_.script_host_apis;
    return desc;
}

ScriptHostCommandRouter GameRuntime::make_script_host_command_router() const {
    return ScriptHostCommandRouter(startup_report_.script_host_command_routes);
}

core::Result<std::unique_ptr<audio::IAudioSystem>>
GameRuntime::create_audio_system(audio::AudioBackend backend, audio::AudioMixerConfig mixer,
                                 bool use_null_output_device,
                                 const assets::CookedAssetStore* cooked_assets) const {
    if (!is_initialized() || assets_ == nullptr || sound_events_ == nullptr) {
        return core::Result<std::unique_ptr<audio::IAudioSystem>>::failure(
            "game_runtime.audio_content_unavailable",
            "game runtime audio content must be initialized before creating audio");
    }
    audio::AudioSystemDesc desc;
    desc.backend = backend;
    desc.events = sound_events_.get();
    desc.assets = assets_.get();
    desc.cooked_assets = cooked_assets;
    desc.mixer = mixer;
    desc.use_null_output_device = use_null_output_device;
    return audio::create_audio_system(desc);
}

const GameSystemDescriptor* GameRuntime::find_system(GameSystemKind kind) const noexcept {
    const auto found =
        std::ranges::find_if(startup_report_.systems, [kind](const GameSystemDescriptor& system) {
            return system.kind == kind;
        });
    return found == startup_report_.systems.end() ? nullptr : &*found;
}

core::Status GameRuntime::require_system(GameSystemKind kind) const {
    if (find_system(kind) == nullptr) {
        return core::Status::failure("game_runtime.missing_system",
                                     "game runtime system is not registered: " +
                                         std::string(game_system_kind_name(kind)));
    }
    return core::Status::ok();
}

core::Status GameRuntime::require_prototype_kind(std::string_view kind) const {
    if (startup_report_.count_kind(kind) == 0) {
        return core::Status::failure("game_runtime.missing_prototype_kind",
                                     missing_kind_message(kind));
    }
    return core::Status::ok();
}

core::Status GameRuntime::start_session(SessionLaunchRequest request) {
    if (!is_initialized() || prototypes_ == nullptr || voxel_palette_ == nullptr) {
        return core::Status::failure("game_runtime.not_initialized",
                                     "game runtime content must be initialized first");
    }
    if (session_ != nullptr) {
        return core::Status::failure("game_runtime.session_active",
                                     "game runtime already owns an active session");
    }
    if (request.ownership_generation == 0) {
        request.ownership_generation = next_session_generation_;
    } else if (request.ownership_generation < next_session_generation_) {
        return core::Status::failure(
            "game_runtime.stale_session_generation",
            "session launch generation is older than a previously owned session");
    }
    if (request.ownership_generation == std::numeric_limits<std::uint64_t>::max()) {
        return core::Status::failure("game_runtime.session_generation_exhausted",
                                     "session ownership generation space is exhausted");
    }
    next_session_generation_ = request.ownership_generation + 1;

    switch (request.mode) {
    case SessionMode::local_single_player:
        request.runtime.create_server = true;
        request.runtime.create_client = true;
        request.runtime.use_in_memory_transport = true;
        request.runtime.remote_server_endpoint.reset();
        break;
    case SessionMode::hosted_multiplayer:
        request.runtime.create_server = true;
        request.runtime.create_client = true;
        request.runtime.use_in_memory_transport = false;
        if (request.network_endpoint.has_value()) {
            request.runtime.server_bind_endpoint = *request.network_endpoint;
        }
        break;
    case SessionMode::remote_multiplayer:
        request.runtime.create_server = false;
        request.runtime.create_client = true;
        request.runtime.use_in_memory_transport = false;
        request.runtime.remote_server_endpoint = request.network_endpoint;
        break;
    case SessionMode::dedicated_server:
        request.runtime.create_server = true;
        request.runtime.create_client = false;
        request.runtime.use_in_memory_transport = false;
        request.runtime.headless = true;
        request.runtime.create_renderer = false;
        request.runtime.create_audio = false;
        if (request.network_endpoint.has_value()) {
            request.runtime.server_bind_endpoint = *request.network_endpoint;
        }
        break;
    case SessionMode::automated:
        break;
    case SessionMode::replay:
        return core::Status::failure("session_launch.replay_unsupported",
                                     "this build has no replay runtime");
    }

    if (request.save_path.has_value() && !request.initial_snapshot.has_value() &&
        request.world_source == WorldSourceKind::existing_save) {
        save::FileSaveDatabase database(*request.save_path);
        auto snapshot = database.read_snapshot();
        if (!snapshot) {
            return core::Status::failure(snapshot.error().code, snapshot.error().message);
        }
        request.metadata = snapshot.value().metadata;
        request.initial_snapshot = std::move(snapshot).value();
    }
    if (request.seed.has_value()) {
        if (request.metadata.world_seed != 0 && request.metadata.world_seed != *request.seed) {
            return core::Status::failure("session_launch.seed_mismatch",
                                         "requested seed does not match prepared world metadata");
        }
        request.metadata.world_seed = *request.seed;
    }
    if (request.scenario_id.empty()) {
        request.scenario_id = startup_report_.selected_scenario_id;
    }
    if (request.initial_snapshot.has_value()) {
        request.metadata = request.initial_snapshot->metadata;
    }
    auto request_status = request.validate();
    if (!request_status) {
        return request_status;
    }
    auto session_palette = voxel_palette_;
    if (request.initial_snapshot.has_value()) {
        auto restored_palette = world::voxel_palette_from_prototypes(
            *prototypes_, request.initial_snapshot->voxel_palette, true);
        if (!restored_palette) {
            return core::Status::failure(restored_palette.error().code,
                                         restored_palette.error().message);
        }
        session_palette =
            std::make_shared<world::VoxelPalette>(std::move(restored_palette).value());
    }
    auto runtime_config = request.runtime;
    auto created = RuntimeSession::create(std::move(runtime_config), std::move(request),
                                          *prototypes_, *session_palette);
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    session_voxel_palette_ = std::move(session_palette);
    session_ = std::move(created).value();
    return core::Status::ok();
}

core::Status GameRuntime::start_session(RuntimeConfiguration config, SessionRequest request) {
    request.runtime = std::move(config);
    request.mode = SessionMode::automated;
    if (request.world_source == WorldSourceKind::generated) {
        request.world_source = WorldSourceKind::automated_scenario;
    }
    return start_session(std::move(request));
}

core::Status GameRuntime::start_session_from_save(RuntimeConfiguration config,
                                                  const save::FileSaveDatabase& database,
                                                  std::string scenario_id) {
    if (!is_initialized() || prototypes_ == nullptr) {
        return core::Status::failure("game_runtime.not_initialized",
                                     "game runtime content must be initialized first");
    }
    // Missing prototypes are recoverable at the world-import boundary, where their opaque save
    // records are preserved as placeholders before the remaining snapshot is validated.
    auto snapshot = database.read_snapshot();
    if (!snapshot) {
        return core::Status::failure(snapshot.error().code, snapshot.error().message);
    }
    SessionRequest request;
    request.metadata = snapshot.value().metadata;
    if (scenario_id.empty()) {
        const auto saved_scenario = std::ranges::find_if(
            snapshot.value().mod_states, [](const save::ModStateSaveRecord& record) {
                return record.mod_id == "engine" && record.state_key == "scenario.id";
            });
        if (saved_scenario != snapshot.value().mod_states.end()) {
            scenario_id = saved_scenario->encoded_state;
        }
    }
    request.scenario_id = std::move(scenario_id);
    request.initial_snapshot = std::move(snapshot).value();
    return start_session(std::move(config), std::move(request));
}

core::Result<RuntimeFrameStats> GameRuntime::run_frame(RuntimeFrameInput input) {
    if (session_ == nullptr) {
        return core::Result<RuntimeFrameStats>::failure(
            "game_runtime.no_session", "game runtime has no active session to advance");
    }
    return session_->run_frame(input);
}

core::Status GameRuntime::submit_command(std::string type, std::string payload,
                                         std::int64_t now_ms) {
    if (session_ == nullptr) {
        return core::Status::failure("game_runtime.no_session",
                                     "game runtime has no active command path");
    }
    return session_->submit_command(std::move(type), std::move(payload), now_ms);
}

core::Result<save::SaveSnapshot> GameRuntime::capture_save_snapshot() const {
    if (session_ == nullptr) {
        return core::Result<save::SaveSnapshot>::failure(
            "game_runtime.no_session", "game runtime has no active session to save");
    }
    return session_->capture_save_snapshot();
}

core::Status GameRuntime::save_to(const save::FileSaveDatabase& database) const {
    if (session_ == nullptr) {
        return core::Status::failure("game_runtime.no_session",
                                     "game runtime has no active session to save");
    }
    return session_->save_to(database);
}

core::Result<RenderSnapshot> GameRuntime::capture_render_snapshot() const {
    if (session_ == nullptr || session_->client() == nullptr) {
        return core::Result<RenderSnapshot>::failure(
            "game_runtime.no_client_presentation",
            "render snapshot extraction requires an active client presentation world");
    }
    return core::Result<RenderSnapshot>::success(session_->capture_render_snapshot());
}

core::Result<debug::InspectionData> GameRuntime::inspect_session() const {
    if (session_ == nullptr) {
        return core::Result<debug::InspectionData>::failure(
            "game_runtime.no_session", "game runtime has no active session to inspect");
    }
    return core::Result<debug::InspectionData>::success(GameInspector::inspect(*session_));
}

core::Result<std::vector<debug::InspectionData>> GameRuntime::inspect_system_timings() const {
    if (session_ == nullptr || session_->server() == nullptr) {
        return core::Result<std::vector<debug::InspectionData>>::failure(
            "game_runtime.no_authoritative_runtime",
            "simulation timing inspection requires an active authoritative runtime");
    }
    return core::Result<std::vector<debug::InspectionData>>::success(
        GameInspector::inspect_system_timings(*session_->server()));
}

core::Status GameRuntime::shutdown() {
    if (session_ == nullptr) {
        return core::Status::ok();
    }
    auto status = session_->shutdown();
    last_teardown_report_ = session_->teardown_report();
    session_.reset();
    session_voxel_palette_.reset();
    return status;
}

const std::optional<SessionTeardownReport>& GameRuntime::last_teardown_report() const noexcept {
    return last_teardown_report_;
}

RuntimeSession* GameRuntime::session() noexcept {
    return session_.get();
}

const RuntimeSession* GameRuntime::session() const noexcept {
    return session_.get();
}

std::string_view game_system_kind_name(GameSystemKind kind) noexcept {
    switch (kind) {
    case GameSystemKind::building:
        return "building";
    case GameSystemKind::rooms:
        return "rooms";
    case GameSystemKind::crafting:
        return "crafting";
    case GameSystemKind::production:
        return "production";
    case GameSystemKind::logistics:
        return "logistics";
    case GameSystemKind::farming:
        return "farming";
    case GameSystemKind::animals:
        return "animals";
    case GameSystemKind::ecology:
        return "ecology";
    case GameSystemKind::combat:
        return "combat";
    case GameSystemKind::sleep:
        return "sleep";
    case GameSystemKind::magic:
        return "magic";
    case GameSystemKind::workpieces:
        return "workpieces";
    }
    return "unknown";
}

} // namespace heartstead::game
