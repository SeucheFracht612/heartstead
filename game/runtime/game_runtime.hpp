#pragma once

#include "engine/assets/asset_catalog.hpp"
#include "engine/audio/audio_system.hpp"
#include "engine/audio/sound_event.hpp"
#include "engine/core/result.hpp"
#include "engine/debug/inspection.hpp"
#include "engine/modding/mod_validation.hpp"
#include "engine/scripting/script_runtime.hpp"
#include "game/runtime/runtime_session.hpp"
#include "game/runtime/script_host_commands.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace heartstead::content {
struct ContentValidationReport;
} // namespace heartstead::content

namespace heartstead::game {

enum class GameSystemKind {
    building,
    rooms,
    crafting,
    production,
    logistics,
    farming,
    animals,
    ecology,
    combat,
    sleep,
    magic,
    workpieces,
};

struct GameSystemDescriptor {
    GameSystemKind kind = GameSystemKind::building;
    std::string name;
    bool server_authoritative = true;
    bool mod_data_driven = true;
};

struct GameRuntimeConfig {
    GameRuntimeConfig();

    std::string base_mod_id = "base";
    bool require_base_mod = true;
    std::string selected_scenario_id = "base:scenarios/homestead";
    bool require_selected_scenario = true;
    std::vector<std::string> required_prototype_kinds;
    bool register_default_script_host_apis = true;
    std::vector<scripting::ScriptHostApiDesc> additional_script_host_apis;
    bool register_default_script_host_command_routes = true;
    std::vector<ScriptHostCommandRoute> additional_script_host_command_routes;
};

struct GameRuntimeStartupReport {
    bool initialized = false;
    bool base_mod_loaded = false;
    std::string base_mod_id;
    std::size_t mod_count = 0;
    std::size_t resource_pack_count = 0;
    std::size_t prototype_count = 0;
    std::size_t active_asset_count = 0;
    std::size_t voxel_palette_entry_count = 0;
    std::size_t material_definition_count = 0;
    std::size_t material_asset_reference_count = 0;
    std::size_t sound_event_count = 0;
    bool aggregate_content_validated = false;
    std::string selected_scenario_id;
    std::string selected_scenario_start_region;
    std::string selected_scenario_spawn_mode;
    std::size_t script_host_api_count = 0;
    std::vector<scripting::ScriptHostApiDesc> script_host_apis;
    std::size_t script_host_command_route_count = 0;
    std::vector<ScriptHostCommandRoute> script_host_command_routes;
    std::unordered_map<std::string, std::size_t> prototypes_by_kind;
    std::vector<GameSystemDescriptor> systems;

    [[nodiscard]] std::size_t count_kind(std::string_view kind) const noexcept;
};

class GameRuntime {
  public:
    GameRuntime();
    ~GameRuntime();
    GameRuntime(const GameRuntime&) = delete;
    GameRuntime& operator=(const GameRuntime&) = delete;
    GameRuntime(GameRuntime&&) noexcept;
    GameRuntime& operator=(GameRuntime&&) noexcept;

    [[nodiscard]] static std::vector<std::string> default_required_prototype_kinds();
    [[nodiscard]] static std::vector<GameSystemDescriptor> default_systems();
    [[nodiscard]] static std::vector<scripting::ScriptHostApiDesc> default_script_host_apis();
    [[nodiscard]] static std::vector<ScriptHostCommandRoute> default_script_host_command_routes();

    [[nodiscard]] static core::Result<GameRuntime>
    initialize(GameRuntimeConfig config, const modding::ModValidationReport& content_report);
    [[nodiscard]] static core::Result<GameRuntime>
    initialize(GameRuntimeConfig config,
               const heartstead::content::ContentValidationReport& content_report);

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const GameRuntimeConfig& config() const noexcept;
    [[nodiscard]] const GameRuntimeStartupReport& startup_report() const noexcept;
    [[nodiscard]] scripting::ScriptRuntimeDesc
    make_script_runtime_desc(scripting::ScriptBackend backend) const;
    [[nodiscard]] ScriptHostCommandRouter make_script_host_command_router() const;
    [[nodiscard]] core::Result<std::unique_ptr<audio::IAudioSystem>>
    create_audio_system(audio::AudioBackend backend, audio::AudioMixerConfig mixer = {},
                        bool use_null_output_device = false) const;

    [[nodiscard]] const GameSystemDescriptor* find_system(GameSystemKind kind) const noexcept;
    [[nodiscard]] core::Status require_system(GameSystemKind kind) const;
    [[nodiscard]] core::Status require_prototype_kind(std::string_view kind) const;
    [[nodiscard]] core::Status start_session(RuntimeConfiguration config, SessionRequest request);
    [[nodiscard]] core::Status start_session_from_save(RuntimeConfiguration config,
                                                       const save::FileSaveDatabase& database,
                                                       std::string scenario_id = {});
    [[nodiscard]] core::Result<RuntimeFrameStats> run_frame(RuntimeFrameInput input);
    [[nodiscard]] core::Status submit_command(std::string type, std::string payload,
                                              std::int64_t now_ms = 0);
    [[nodiscard]] core::Result<save::SaveSnapshot> capture_save_snapshot() const;
    [[nodiscard]] core::Status save_to(const save::FileSaveDatabase& database) const;
    [[nodiscard]] core::Result<RenderSnapshot> capture_render_snapshot() const;
    [[nodiscard]] core::Result<debug::InspectionData> inspect_session() const;
    [[nodiscard]] core::Result<std::vector<debug::InspectionData>> inspect_system_timings() const;
    [[nodiscard]] core::Status shutdown();
    [[nodiscard]] RuntimeSession* session() noexcept;
    [[nodiscard]] const RuntimeSession* session() const noexcept;

  private:
    GameRuntimeConfig config_;
    GameRuntimeStartupReport startup_report_;
    std::shared_ptr<const modding::PrototypeRegistry> prototypes_;
    std::shared_ptr<const world::VoxelPalette> voxel_palette_;
    std::shared_ptr<const assets::AssetCatalog> assets_;
    std::shared_ptr<const audio::SoundEventRegistry> sound_events_;
    std::unique_ptr<RuntimeSession> session_;
};

[[nodiscard]] std::string_view game_system_kind_name(GameSystemKind kind) noexcept;

} // namespace heartstead::game
