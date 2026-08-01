#include "game/application/heartstead_application_mode.hpp"

#include "engine/assets/cooked_asset_store.hpp"
#include "engine/audio/audio_system.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/logging.hpp"
#include "engine/input/input_action.hpp"
#include "engine/movement/player_camera.hpp"
#include "engine/save/save_compatibility.hpp"
#include "engine/save/save_slot.hpp"
#include "engine/ui/widget_tree.hpp"
#include "game/application/application_state.hpp"
#include "game/application/main_menu.hpp"
#include "game/application/runtime_diagnostics.hpp"
#include "game/features/animals/wandering_animal_module.hpp"
#include "game/features/interaction/voxel_raycast.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/presentation/client_audio_presentation.hpp"
#include "game/presentation/model_presentation_system.hpp"
#include "game/runtime/game_runtime.hpp"
#include "game/scenarios/developer_world_registry.hpp"
#include "game/ui/game_ui.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <exception>
#include <future>
#include <iomanip>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <stop_token>
#include <string>
#include <utility>

namespace heartstead::game {

namespace {

const auto root_id = ui::widget_id("heartstead.shell.root");
const auto panel_id = ui::widget_id("heartstead.shell.panel");
const auto continue_id = ui::widget_id("heartstead.menu.continue");
const auto new_world_id = ui::widget_id("heartstead.menu.new_world");
const auto load_world_id = ui::widget_id("heartstead.menu.load_world");
const auto multiplayer_id = ui::widget_id("heartstead.menu.multiplayer");
const auto developer_worlds_id = ui::widget_id("heartstead.menu.developer_worlds");
const auto options_id = ui::widget_id("heartstead.menu.options");
const auto quit_id = ui::widget_id("heartstead.menu.quit");
const auto resume_id = ui::widget_id("heartstead.pause.resume");
const auto return_id = ui::widget_id("heartstead.pause.return");
const auto cancel_id = ui::widget_id("heartstead.loading.cancel");
const auto back_id = ui::widget_id("heartstead.error.back");
const auto menu_back_id = ui::widget_id("heartstead.menu.back");
const auto world_name_id = ui::widget_id("heartstead.new_world.name");
const auto world_seed_id = ui::widget_id("heartstead.new_world.seed");
const auto create_world_id = ui::widget_id("heartstead.new_world.create");
const auto load_selected_id = ui::widget_id("heartstead.load_world.load");
const auto host_selected_id = ui::widget_id("heartstead.load_world.host");
const auto rename_selected_id = ui::widget_id("heartstead.load_world.rename");
const auto rename_world_name_id = ui::widget_id("heartstead.rename_world.name");
const auto rename_confirm_id = ui::widget_id("heartstead.rename_world.confirm");
const auto duplicate_selected_id = ui::widget_id("heartstead.load_world.duplicate");
const auto migrate_selected_id = ui::widget_id("heartstead.load_world.migrate");
const auto delete_selected_id = ui::widget_id("heartstead.load_world.delete");
const auto refresh_worlds_id = ui::widget_id("heartstead.load_world.refresh");
const auto copy_save_path_id = ui::widget_id("heartstead.load_world.copy_path");
const auto address_id = ui::widget_id("heartstead.multiplayer.address");
const auto host_address_id = ui::widget_id("heartstead.multiplayer.host_address");
const auto join_id = ui::widget_id("heartstead.multiplayer.join");
const auto developer_launch_id = ui::widget_id("heartstead.developer.launch");
const auto developer_search_id = ui::widget_id("heartstead.developer.search");
const auto developer_category_id = ui::widget_id("heartstead.developer.category");
const auto delete_confirm_id = ui::widget_id("heartstead.delete.confirm");
const auto delete_cancel_id = ui::widget_id("heartstead.delete.cancel");
const auto master_volume_id = ui::widget_id("heartstead.options.master_volume");
const auto music_volume_id = ui::widget_id("heartstead.options.music_volume");
const auto effects_volume_id = ui::widget_id("heartstead.options.effects_volume");
const auto mouse_sensitivity_id = ui::widget_id("heartstead.options.mouse_sensitivity");
const auto ui_scale_id = ui::widget_id("heartstead.options.ui_scale");
const auto contrast_id = ui::widget_id("heartstead.options.contrast");
const auto saturation_id = ui::widget_id("heartstead.options.saturation");
const auto quality_id = ui::widget_id("heartstead.options.quality");
const auto windowed_id = ui::widget_id("heartstead.options.windowed");
const auto vsync_id = ui::widget_id("heartstead.options.vsync");
const auto color_vision_id = ui::widget_id("heartstead.options.color_vision");

[[nodiscard]] ui::WidgetDesc root_widget() {
    ui::WidgetDesc root;
    root.id = root_id;
    root.kind = ui::WidgetKind::panel;
    root.layout.mode = ui::UiLayoutMode::overlay;
    root.layout.width = ui::UiSize::fill();
    root.layout.height = ui::UiSize::fill();
    root.color = {0.025F, 0.045F, 0.065F, 0.94F};
    root.blocks_gameplay = true;
    return root;
}

[[nodiscard]] ui::WidgetDesc menu_panel() {
    ui::WidgetDesc panel;
    panel.id = panel_id;
    panel.parent = root_id;
    panel.kind = ui::WidgetKind::scroll_area;
    panel.nine_slice = "carved_panel";
    panel.layout.mode = ui::UiLayoutMode::column;
    panel.layout.width = ui::UiSize::pixels(520.0F);
    panel.layout.height = ui::UiSize::content();
    panel.layout.minimum_height = 260.0F;
    panel.layout.maximum_height = 620.0F;
    panel.layout.padding = {32.0F, 30.0F, 32.0F, 30.0F};
    panel.layout.gap = 14.0F;
    panel.layout.horizontal_alignment = ui::UiAlignment::center;
    panel.layout.vertical_alignment = ui::UiAlignment::center;
    panel.color = {0.25F, 0.13F, 0.045F, 0.98F};
    return panel;
}

[[nodiscard]] ui::WidgetDesc label(std::string_view stable_id, std::string text,
                                   float glyph_size = 18.0F) {
    ui::WidgetDesc result;
    result.id = ui::widget_id(stable_id);
    result.parent = panel_id;
    result.kind = ui::WidgetKind::label;
    result.layout.width = ui::UiSize::fill();
    result.layout.height = ui::UiSize::content();
    result.layout.padding = {8.0F, 5.0F, 8.0F, 5.0F};
    result.text = std::move(text);
    result.glyph_size_pixels = glyph_size;
    return result;
}

[[nodiscard]] ui::WidgetDesc button(ui::WidgetId id, std::string text, std::string tooltip = {},
                                    bool enabled = true) {
    ui::WidgetDesc result;
    result.id = id;
    result.parent = panel_id;
    result.kind = ui::WidgetKind::button;
    result.nine_slice = "carved_button";
    result.layout.width = ui::UiSize::fill();
    result.layout.height = ui::UiSize::pixels(48.0F);
    result.layout.padding = {16.0F, 14.0F, 16.0F, 8.0F};
    result.text = std::move(text);
    result.tooltip = std::move(tooltip);
    result.glyph_size_pixels = 16.0F;
    result.enabled = enabled;
    result.focusable = enabled;
    result.pointer_events = enabled;
    result.color = {0.42F, 0.23F, 0.075F, 1.0F};
    return result;
}

[[nodiscard]] ui::WidgetDesc text_input(ui::WidgetId id, std::string text,
                                        std::string accessibility_label) {
    auto result = button(id, std::move(text));
    result.kind = ui::WidgetKind::text_input;
    result.accessibility_label = std::move(accessibility_label);
    return result;
}

[[nodiscard]] ui::WidgetDesc slider(ui::WidgetId id, std::string accessibility_label, float value,
                                    float minimum, float maximum) {
    auto result = button(id, std::move(accessibility_label));
    result.kind = ui::WidgetKind::slider;
    result.value = value;
    result.minimum_value = minimum;
    result.maximum_value = maximum;
    return result;
}

[[nodiscard]] ui::WidgetDesc toggle(ui::WidgetId id, std::string text, bool checked) {
    auto result = button(id, std::move(text));
    result.kind = ui::WidgetKind::toggle;
    result.checked = checked;
    return result;
}

[[nodiscard]] std::string timestamp_text(std::uint64_t milliseconds) {
    if (milliseconds == 0) {
        return "unknown";
    }
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000U);
    std::tm utc{};
#if defined(_WIN32)
    if (gmtime_s(&utc, &seconds) != 0) {
#else
    if (gmtime_r(&seconds, &utc) == nullptr) {
#endif
        return "unknown";
    }
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%d %H:%M UTC");
    return output.str();
}

[[nodiscard]] std::string required_mods_text(const save::SaveMetadata& metadata) {
    if (metadata.enabled_mods.empty()) {
        return "none";
    }
    std::string result;
    for (const auto& mod : metadata.enabled_mods) {
        if (!result.empty()) {
            result += ", ";
        }
        result += mod.id + " " + mod.version;
    }
    return result;
}

[[nodiscard]] bool key_pressed(const platform::WindowInputSnapshot& input,
                               platform::KeyCode key) noexcept {
    return std::ranges::find(input.pressed_keys, key) != input.pressed_keys.end();
}

[[nodiscard]] std::optional<scenarios::ScenarioCategory>
next_scenario_category(std::optional<scenarios::ScenarioCategory> category) noexcept {
    constexpr std::array categories{
        scenarios::ScenarioCategory::gameplay,    scenarios::ScenarioCategory::rendering,
        scenarios::ScenarioCategory::world,       scenarios::ScenarioCategory::movement,
        scenarios::ScenarioCategory::physics,     scenarios::ScenarioCategory::networking,
        scenarios::ScenarioCategory::performance, scenarios::ScenarioCategory::audio,
        scenarios::ScenarioCategory::ui,
    };
    if (!category.has_value()) {
        return categories.front();
    }
    const auto found = std::ranges::find(categories, *category);
    return found == categories.end() || std::next(found) == categories.end()
               ? std::nullopt
               : std::optional{*std::next(found)};
}

[[nodiscard]] renderer::RendererQualityPreset
next_quality(renderer::RendererQualityPreset quality) noexcept {
    switch (quality) {
    case renderer::RendererQualityPreset::low:
        return renderer::RendererQualityPreset::medium;
    case renderer::RendererQualityPreset::medium:
        return renderer::RendererQualityPreset::high;
    case renderer::RendererQualityPreset::high:
        return renderer::RendererQualityPreset::ultra;
    case renderer::RendererQualityPreset::ultra:
        return renderer::RendererQualityPreset::low;
    }
    return renderer::RendererQualityPreset::high;
}

[[nodiscard]] std::string_view color_vision_name(renderer::UiColorVisionMode mode) noexcept {
    switch (mode) {
    case renderer::UiColorVisionMode::none:
        return "none";
    case renderer::UiColorVisionMode::protanopia:
        return "protanopia";
    case renderer::UiColorVisionMode::deuteranopia:
        return "deuteranopia";
    case renderer::UiColorVisionMode::tritanopia:
        return "tritanopia";
    }
    return "none";
}

[[nodiscard]] renderer::UiColorVisionMode
next_color_vision(renderer::UiColorVisionMode mode) noexcept {
    switch (mode) {
    case renderer::UiColorVisionMode::none:
        return renderer::UiColorVisionMode::protanopia;
    case renderer::UiColorVisionMode::protanopia:
        return renderer::UiColorVisionMode::deuteranopia;
    case renderer::UiColorVisionMode::deuteranopia:
        return renderer::UiColorVisionMode::tritanopia;
    case renderer::UiColorVisionMode::tritanopia:
        return renderer::UiColorVisionMode::none;
    }
    return renderer::UiColorVisionMode::none;
}

[[nodiscard]] renderer::RenderCamera camera_from_frame(const movement::PlayerCameraFrame& frame) {
    renderer::RenderCamera camera;
    camera.floating_origin = frame.floating_origin;
    camera.local_position = {static_cast<float>(frame.position.local_offset.x),
                             static_cast<float>(frame.position.local_offset.y),
                             static_cast<float>(frame.position.local_offset.z)};
    camera.view = frame.view;
    camera.projection = frame.projection;
    camera.view_projection = frame.view_projection;
    return camera;
}

struct LoadedSession {
    std::uint64_t generation = 0;
    SessionMode mode = SessionMode::local_single_player;
    PersistencePolicy persistence = PersistencePolicy::ephemeral;
    std::string save_slot_id;
    std::string world_name;
    GameRuntime runtime;
};

struct LoadingProgressState {
    std::atomic<SessionStartupPhase> phase{SessionStartupPhase::validating_request};
};

using SessionLoadResult = core::Result<LoadedSession>;

} // namespace

struct HeartsteadApplicationMode::Impl final : IApplicationStateLifecycle {
    explicit Impl(HeartsteadApplicationModeConfig initial_config)
        : config(std::move(initial_config)), states(this),
          widgets(config.content_report == nullptr ? ui::UiSkin::storybook_default()
                                                   : config.content_report->ui_skin),
          settings(config.initial_settings), settings_store(config.user_data_root / "settings.txt"),
          save_catalog(config.user_data_root / "saves") {
        camera_perspective = settings.first_person_camera
                                 ? movement::PlayerCameraPerspective::first_person
                                 : movement::PlayerCameraPerspective::third_person;
    }

    HeartsteadApplicationModeConfig config;
    ApplicationStateMachine states;
    GameApplicationServices* services = nullptr;
    const GameApplicationFrame* frame = nullptr;
    GameRuntime application_runtime;
    std::optional<GameRuntime> session_runtime;
    std::future<SessionLoadResult> loading;
    std::optional<jobs::JobId> loading_job;
    std::stop_source loading_stop;
    std::shared_ptr<LoadingProgressState> loading_progress;
    std::optional<SessionStartupPhase> rendered_loading_phase;
    ui::WidgetTree widgets;
    ApplicationSettings settings;
    ApplicationSettingsStore settings_store;
    save::FileSaveSlotCatalog save_catalog;
    DeveloperWorldRegistry developer_worlds;
    std::vector<save::SaveSlotSummary> save_entries;
    MainMenuNavigation menu_navigation;
    std::optional<std::size_t> selected_save;
    std::optional<std::size_t> selected_developer_world;
    std::optional<scenarios::ScenarioCategory> developer_category;
    std::optional<std::string> pending_delete_slot;
    std::string pending_delete_name;
    std::optional<SessionLaunchRequest> pending_launch;
    std::string pending_save_slot;
    std::string active_save_slot;
    std::optional<std::filesystem::path> active_save_path;
    std::string active_world_name;
    SessionMode active_session_mode = SessionMode::local_single_player;
    PersistencePolicy active_persistence = PersistencePolicy::ephemeral;
    std::string new_world_name = "New Homestead";
    std::string new_world_seed = std::to_string(foundation::world_seed);
    std::string rename_world_name;
    std::string server_address = "127.0.0.1:7777";
    std::string host_address = "0.0.0.0:7777";
    std::string developer_search;
    std::string menu_message;
    movement::PlayerCameraRig camera_rig;
    movement::PlayerCameraPerspective camera_perspective =
        movement::PlayerCameraPerspective::third_person;
    movement::FixedStepPlayerInputScheduler input_scheduler;
    input::InputActionMap actions = input::InputActionMap::gameplay_defaults();
    bool input_orientation_initialized = false;
    bool gameplay_input_enabled = true;
    std::unique_ptr<GameUiLayer> game_ui;
    ClientAudioPresentation audio_presentation;
    bool audio_presentation_initialized = false;
    ModelPresentationSystem model_presentation;
    bool model_presentation_initialized = false;
    std::optional<movement::PlayerCameraFrame> player_camera_frame;
    std::optional<RuntimeFrameStats> runtime_stats;
    std::uint64_t frame_count = 0;
    std::uint64_t completed_session_count = 0;
    std::uint64_t next_session_generation = 1;
    std::uint64_t loading_generation = 0;
    std::int64_t last_runtime_time_ms = 0;
    std::int64_t last_wall_clock_ms = 0;
    std::int64_t last_autosave_at_ms = 0;
    std::int64_t settings_persist_after_ms = 0;
    std::uint64_t periodic_save_count = 0;
    SessionMode loading_mode = SessionMode::local_single_player;
    bool initialized = false;
    bool diagnostics_visible = false;
    bool settings_persist_pending = false;
    std::optional<core::Error> display_error;

    [[nodiscard]] RuntimeDiagnosticsSnapshot diagnostics_snapshot() const {
        RuntimeDiagnosticsSnapshot snapshot;
        snapshot.application_state = states.state();
        snapshot.active_world = active_world_name;
        if (active_save_path.has_value()) {
            snapshot.save_destination = active_save_path->string();
        }
        snapshot.pending_loading_operations = loading.valid() ? 1U : 0U;
        if (loading_progress != nullptr && loading.valid()) {
            snapshot.loading_phase = loading_progress->phase.load(std::memory_order_relaxed);
        }
        if (services != nullptr && services->jobs() != nullptr) {
            snapshot.active_jobs = services->jobs()->pending_count();
        }
        if (runtime_stats.has_value()) {
            snapshot.authoritative_tick = runtime_stats->authoritative_world_tick;
            snapshot.interpolation_alpha = runtime_stats->fixed_step.interpolation_alpha;
            snapshot.dropped_tick_time_us = runtime_stats->fixed_step.dropped_time_us;
        }
        if (session_runtime.has_value() && session_runtime->session() != nullptr) {
            const auto* session = session_runtime->session();
            snapshot.session_state = session->state();
            snapshot.session_mode = session->launch_request().mode;
            snapshot.connection_state = session->connection_state();
            snapshot.session_generation = session->ownership_generation();
            snapshot.fixed_step_tick = session->fixed_step_tick();
            const auto resources = session->resource_counts();
            snapshot.world_entities = resources.server_entities;
            snapshot.physics_objects = resources.physics_bodies;
            snapshot.presentation_objects = resources.presentation_objects;
            snapshot.registered_session_callbacks = resources.registered_cleanup_callbacks;
            snapshot.active_jobs += resources.active_jobs;
            if (session->connection_state() == SessionConnectionState::connecting ||
                session->connection_state() == SessionConnectionState::connected) {
                snapshot.active_network_connections = 1;
            }
        }
        if (services != nullptr && services->renderer() != nullptr) {
            const auto& render = services->renderer()->stats();
            snapshot.render_objects = render.retained_objects;
            snapshot.asset_references =
                static_cast<std::size_t>(render.resident_textures) + render.resident_static_meshes;
            snapshot.resident_gpu_bytes =
                render.resident_texture_bytes + render.resident_mesh_bytes +
                render.resident_static_mesh_bytes + render.far_terrain_resident_bytes;
            if (render.device_memory_budget_valid) {
                snapshot.device_gpu_usage_bytes = render.device_local_memory_usage_bytes;
                snapshot.device_gpu_budget_bytes = render.device_local_memory_budget_bytes;
            }
        }
        if (services != nullptr && services->audio() != nullptr) {
            const auto audio = services->audio()->stats();
            snapshot.audio_emitters = audio.active_voices;
            snapshot.asset_references += audio.cached_assets;
        }
        snapshot.process = sample_process_resources();
        return snapshot;
    }

    [[nodiscard]] bool save_is_compatible(const save::SaveSlotSummary& entry) const {
        if (entry.validation_error.has_value() || !entry.snapshot_metadata.has_value()) {
            return false;
        }
        if (entry.snapshot_metadata->schema_version != save::current_save_schema_version) {
            return false;
        }
        return !save::SaveCompatibilityChecker::compare(entry.snapshot_metadata.value(),
                                                        config.content_report->mod_fingerprints)
                    .has_errors();
    }

    [[nodiscard]] core::Status refresh_saves(std::string preferred_slot = {}) {
        if (preferred_slot.empty() && selected_save.has_value() &&
            *selected_save < save_entries.size()) {
            preferred_slot = save_entries[*selected_save].slot_id;
        }
        auto listed = save_catalog.list_slots();
        if (!listed) {
            return core::Status::failure(listed.error().code, listed.error().message);
        }
        save_entries = std::move(listed).value();
        selected_save.reset();
        if (!preferred_slot.empty()) {
            const auto selected = std::ranges::find(save_entries, preferred_slot,
                                                    &save::SaveSlotSummary::slot_id);
            if (selected != save_entries.end()) {
                selected_save =
                    static_cast<std::size_t>(std::distance(save_entries.begin(), selected));
            }
        }
        return core::Status::ok();
    }

    [[nodiscard]] std::optional<std::size_t> continue_index() const {
        if (!settings.last_world_slot.empty()) {
            for (std::size_t index = 0; index < save_entries.size(); ++index) {
                if (save_entries[index].slot_id == settings.last_world_slot &&
                    save_is_compatible(save_entries[index])) {
                    return index;
                }
            }
        }
        std::optional<std::size_t> newest;
        for (std::size_t index = 0; index < save_entries.size(); ++index) {
            if (!save_is_compatible(save_entries[index])) {
                continue;
            }
            if (!newest.has_value() || save_entries[index].metadata.last_saved_at_ms >
                                           save_entries[*newest].metadata.last_saved_at_ms) {
                newest = index;
            }
        }
        return newest;
    }

    [[nodiscard]] core::Status persist_settings() {
        auto status = settings_store.save(settings);
        if (!status) {
            menu_message = status.error().code + ": " + status.error().message;
        } else {
            settings_persist_pending = false;
        }
        return status;
    }

    [[nodiscard]] std::uint64_t persisted_timestamp_ms() const noexcept {
        return static_cast<std::uint64_t>(std::max<std::int64_t>(1, last_wall_clock_ms));
    }

    [[nodiscard]] core::Status apply_settings() {
        if (services != nullptr && services->audio() != nullptr) {
            auto status =
                services->audio()->set_bus_gain(audio::AudioBus::master, settings.master_volume);
            if (!status)
                return status;
            status = services->audio()->set_bus_gain(audio::AudioBus::music, settings.music_volume);
            if (!status)
                return status;
            status = services->audio()->set_bus_gain(audio::AudioBus::sfx, settings.effects_volume);
            if (!status)
                return status;
            status =
                services->audio()->set_bus_gain(audio::AudioBus::ambient, settings.effects_volume);
            if (!status)
                return status;
        }
        if (services != nullptr && services->renderer() != nullptr &&
            services->renderer()->ui_renderer() != nullptr) {
            return services->renderer()->ui_renderer()->set_accessibility_settings(
                {settings.ui_contrast, settings.ui_saturation, settings.color_vision_mode});
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Status show_menu(MainMenuScreen screen) {
        auto status = menu_navigation.open(screen);
        if (!status) {
            return status;
        }
        menu_message.clear();
        return rebuild_ui(ApplicationState::main_menu);
    }

    [[nodiscard]] core::Status build_menu_ui() {
        const auto add = [this](ui::WidgetDesc widget) { return widgets.add(std::move(widget)); };
        auto status = core::Status::ok();
        const auto add_title = [&](std::string_view id, std::string title) {
            return add(label(id, std::move(title), 30.0F));
        };
        const auto menu_screen = menu_navigation.screen();
        if (menu_screen == MainMenuScreen::root) {
            const auto recent = continue_index();
            const auto continue_text =
                recent.has_value() ? "Continue — " + save_entries[*recent].metadata.display_name
                                   : "Continue — no compatible world";
            if (!(status = add_title("heartstead.menu.title", "HEARTSTEAD")) ||
                !(status = add(button(continue_id, continue_text,
                                      recent.has_value() ? "Continue the most recently played world"
                                                         : "No compatible recent world exists",
                                      recent.has_value()))) ||
                !(status = add(button(new_world_id, "New World"))) ||
                !(status = add(button(load_world_id, "Load World"))) ||
                !(status = add(button(multiplayer_id, "Multiplayer"))) ||
                !(status = add(button(developer_worlds_id, "Developer Worlds"))) ||
                !(status = add(button(options_id, "Options"))) ||
                !(status = add(button(quit_id, "Quit")))) {
                return status;
            }
            widgets.set_focus(recent.has_value() ? continue_id : new_world_id);
        } else if (menu_screen == MainMenuScreen::new_world) {
            if (!(status = add_title("heartstead.new.title", "New World")) ||
                !(status = add(text_input(world_name_id, new_world_name, "World name"))) ||
                !(status = add(text_input(world_seed_id, new_world_seed, "World seed"))) ||
                !(status = add(label(
                      "heartstead.new.preset",
                      "Generator: temperate_valley | Seed: blank=random, integer, hex, or text"))) ||
                !(status = add(button(create_world_id, "Create World"))) ||
                !(status = add(button(menu_back_id, "Back")))) {
                return status;
            }
            widgets.set_focus(world_name_id);
        } else if (menu_screen == MainMenuScreen::load_world) {
            if (!(status = add_title("heartstead.load.title", "Load World")))
                return status;
            if (save_entries.empty()) {
                if (!(status = add(label("heartstead.load.empty", "No saved worlds found."))))
                    return status;
            }
            for (std::size_t index = 0; index < save_entries.size(); ++index) {
                const auto& entry = save_entries[index];
                const auto compatible = save_is_compatible(entry);
                const auto suffix = entry.validation_error.has_value()
                                        ? " — corrupt: " + entry.validation_error->code
                                    : compatible ? " — compatible"
                                                 : " — incompatible or missing content";
                const auto id = ui::widget_id("heartstead.save." + entry.slot_id);
                if (!(status = add(button(id, entry.metadata.display_name + suffix,
                                          "Select this saved world"))))
                    return status;
            }
            const auto has_selection = selected_save.has_value();
            const auto loadable = has_selection && save_is_compatible(save_entries[*selected_save]);
            auto migration_available = false;
            if (has_selection) {
                const auto& entry = save_entries[*selected_save];
                if (!(status =
                          add(label("heartstead.load.selected_dates",
                                    "Created: " + timestamp_text(entry.metadata.created_at_ms) +
                                        " | Last saved: " +
                                        timestamp_text(entry.metadata.last_saved_at_ms)))))
                    return status;
                if (entry.snapshot_metadata.has_value()) {
                    const auto& metadata = *entry.snapshot_metadata;
                    migration_available =
                        metadata.schema_version < save::current_save_schema_version;
                    const auto migration =
                        migration_available
                            ? "required — migrate a copy below"
                        : metadata.schema_version > save::current_save_schema_version
                            ? "save is newer than this build"
                            : "not required";
                    if (!(status = add(label("heartstead.load.selected_version",
                                             "Game: " + metadata.game_version + " | Save schema: " +
                                                 std::to_string(metadata.schema_version) +
                                                 " | Migration: " + migration))) ||
                        !(status =
                              add(label("heartstead.load.selected_generator",
                                        "Generator preset: " +
                                            (entry.generator_preset.empty()
                                                 ? std::string("legacy save (not recorded)")
                                                 : entry.generator_preset)))) ||
                        !(status = add(label("heartstead.load.selected_mods",
                                             "Required mods: " + required_mods_text(metadata)))))
                        return status;
                    const auto compatibility = save::SaveCompatibilityChecker::compare(
                        metadata, config.content_report->mod_fingerprints);
                    for (std::size_t issue = 0; issue < compatibility.issues.size() && issue < 3;
                         ++issue) {
                        if (!(status = add(
                                  label("heartstead.load.compatibility." + std::to_string(issue),
                                        compatibility.issues[issue].message))))
                            return status;
                    }
                } else if (entry.validation_error.has_value() &&
                           !(status = add(
                                 label("heartstead.load.selected_error",
                                       "Validation failed: " + entry.validation_error->message)))) {
                    return status;
                }
            }
            if (!(status = add(button(load_selected_id, "Load Selected", "", loadable))) ||
                !(status = add(button(host_selected_id, "Host Selected", "", loadable))) ||
                !(status = add(button(rename_selected_id, "Rename Selected", "", has_selection))) ||
                !(status =
                      add(button(duplicate_selected_id, "Duplicate Selected", "", loadable))) ||
                !(status = add(button(migrate_selected_id, "Migrate a Copy", "",
                                      migration_available))) ||
                !(status = add(button(delete_selected_id, "Delete Selected", "", has_selection))) ||
                !(status =
                      add(button(copy_save_path_id, "Copy Save Location", "", has_selection))) ||
                !(status = add(text_input(host_address_id, host_address,
                                           "Host bind address and port"))) ||
                !(status = add(button(refresh_worlds_id, "Refresh"))) ||
                !(status = add(button(menu_back_id, "Back"))))
                return status;
            widgets.set_focus(
                selected_save.has_value()
                    ? ui::widget_id("heartstead.save." + save_entries[*selected_save].slot_id)
                : save_entries.empty()
                    ? refresh_worlds_id
                    : ui::widget_id("heartstead.save." + save_entries.front().slot_id));
        } else if (menu_screen == MainMenuScreen::multiplayer) {
            if (!(status = add_title("heartstead.multiplayer.title", "Multiplayer")) ||
                !(status = add(text_input(address_id, server_address, "Server address"))) ||
                !(status = add(button(join_id, "Join by Address"))) ||
                !(status = add(label("heartstead.multiplayer.recent", "Recent servers"))))
                return status;
            for (std::size_t index = 0; index < settings.recent_servers.size(); ++index) {
                if (!(status = add(
                          button(ui::widget_id("heartstead.recent_server." + std::to_string(index)),
                                 settings.recent_servers[index]))))
                    return status;
            }
            if (!(status = add(button(ui::widget_id("heartstead.multiplayer.host_hint"),
                                      "Host via Load World",
                                      "Select Host Selected in the Load World browser", false))) ||
                !(status = add(button(menu_back_id, "Back"))))
                return status;
            widgets.set_focus(address_id);
        } else if (menu_screen == MainMenuScreen::developer_worlds) {
            if (!(status = add_title("heartstead.developer.title", "Developer Worlds")) ||
                !(status = add(text_input(developer_search_id, developer_search, "Search"))) ||
                !(status =
                      add(button(developer_category_id,
                                 "Category: " + std::string(developer_category.has_value()
                                                                ? scenarios::scenario_category_name(
                                                                      *developer_category)
                                                                : std::string_view{"all"})))))
                return status;
            const auto entries = developer_worlds.filter(developer_category, developer_search);
            if (entries.empty() &&
                !(status = add(label("heartstead.developer.empty", "No scenarios match."))))
                return status;
            for (const auto* entry : entries) {
                const auto text =
                    entry->display_name + " — " +
                    std::string(scenarios::scenario_category_name(entry->category)) + " — " +
                    std::string(scenarios::scenario_persistence_policy_name(entry->persistence));
                if (!(status = add(button(
                          ui::widget_id("heartstead.developer." + entry->prototype_id.value()),
                          text, entry->description))))
                    return status;
            }
            const auto selected = selected_developer_world.has_value();
            if (!(status = add(button(developer_launch_id, "Launch Selected", "", selected))) ||
                !(status = add(button(menu_back_id, "Back"))))
                return status;
            widgets.set_focus(developer_search_id);
        } else if (menu_screen == MainMenuScreen::options) {
            const auto displayed_quality = config.safe_mode ? renderer::RendererQualityPreset::low
                                                            : settings.rendering_quality;
            if (!(status = add_title("heartstead.options.title", "Options")) ||
                !(status = add(label("heartstead.options.display",
                                     "Display: " + std::to_string(settings.window_width) + "x" +
                                         std::to_string(settings.window_height) +
                                         " (resize the window to change)"))) ||
                !(status = add(button(
                      quality_id,
                      "Rendering quality: " +
                          std::string(renderer::renderer_quality_preset_name(displayed_quality)),
                      config.safe_mode ? "Safe mode forces low quality" : "Applies on restart",
                      !config.safe_mode))) ||
                !(status = add(toggle(windowed_id, "Windowed mode (restart required)",
                                      settings.windowed))) ||
                !(status = add(toggle(vsync_id, "VSync (restart required)", settings.vsync))) ||
                !(status = add(slider(master_volume_id, "Master volume", settings.master_volume,
                                      0.0F, 1.0F))) ||
                !(status = add(slider(music_volume_id, "Music volume", settings.music_volume, 0.0F,
                                      1.0F))) ||
                !(status = add(slider(effects_volume_id, "Effects volume", settings.effects_volume,
                                      0.0F, 1.0F))) ||
                !(status = add(slider(mouse_sensitivity_id, "Mouse sensitivity",
                                      settings.mouse_sensitivity, 0.1F, 10.0F))) ||
                !(status = add(slider(ui_scale_id, "UI scale", settings.ui_scale, 0.75F, 2.0F))) ||
                !(status =
                      add(slider(contrast_id, "UI contrast", settings.ui_contrast, 0.5F, 2.0F))) ||
                !(status = add(slider(saturation_id, "UI saturation", settings.ui_saturation, 0.0F,
                                      2.0F))) ||
                !(status = add(button(color_vision_id,
                                      "Color vision mode: " + std::string(color_vision_name(
                                                                  settings.color_vision_mode))))) ||
                !(status = add(button(menu_back_id, "Back"))))
                return status;
            widgets.set_focus(master_volume_id);
        } else if (menu_screen == MainMenuScreen::rename_world) {
            if (!(status = add_title("heartstead.rename.title", "Rename World")) ||
                !(status = add(text_input(rename_world_name_id, rename_world_name,
                                           "New world name"))) ||
                !(status = add(button(rename_confirm_id, "Save Name"))) ||
                !(status = add(button(menu_back_id, "Cancel"))))
                return status;
            widgets.set_focus(rename_world_name_id);
        } else if (menu_screen == MainMenuScreen::delete_confirmation) {
            const auto name = pending_delete_name.empty() ? "unknown" : pending_delete_name;
            if (!(status = add_title("heartstead.delete.title", "Delete World?")) ||
                !(status = add(label("heartstead.delete.warning",
                                     "Permanently delete save slot '" + name + "'?"))) ||
                !(status = add(button(delete_confirm_id, "Delete Permanently"))) ||
                !(status = add(button(delete_cancel_id, "Cancel"))))
                return status;
            widgets.set_focus(delete_cancel_id);
        }
        if (!menu_message.empty()) {
            status = add(label("heartstead.menu.message", menu_message, 13.0F));
        }
        return status;
    }

    [[nodiscard]] core::Status rebuild_ui(ApplicationState state) {
        widgets.clear();
        if (config.headless || state == ApplicationState::in_game ||
            state == ApplicationState::shutdown) {
            return core::Status::ok();
        }
        auto status = widgets.add(root_widget());
        if (status) {
            status = widgets.add(menu_panel());
        }
        if (!status) {
            return status;
        }

        const auto add = [this](ui::WidgetDesc widget) { return widgets.add(std::move(widget)); };
        switch (state) {
        case ApplicationState::boot:
            if (!(status = add(label("heartstead.boot.title", "HEARTSTEAD", 32.0F)))) {
                return status;
            }
            return add(label("heartstead.boot.status", "Preparing application services..."));
        case ApplicationState::main_menu:
            return build_menu_ui();
        case ApplicationState::session_loading: {
            const auto phase = loading_progress == nullptr
                                   ? SessionStartupPhase::validating_request
                                   : loading_progress->phase.load(std::memory_order_relaxed);
            rendered_loading_phase = phase;
            if (!(status = add(label("heartstead.loading.title", "Loading world", 28.0F))) ||
                !(status = add(label("heartstead.loading.phase",
                                     std::string(session_startup_phase_name(phase)) + "...")))) {
                return status;
            }
        }
            status = add(button(cancel_id, "Cancel"));
            if (status) {
                widgets.set_focus(cancel_id);
            }
            return status;
        case ApplicationState::paused:
            if (!(status = add(label("heartstead.pause.title", "Paused", 30.0F))) ||
                !(status = add(label("heartstead.pause.behavior",
                                     session_mode_is_multiplayer(active_session_mode)
                                         ? "Multiplayer world continues while this menu is open."
                                         : "Local authoritative simulation is paused."))) ||
                !(status = add(button(resume_id, "Resume")))) {
                return status;
            }
            if (display_error.has_value() &&
                !(status =
                      add(label("heartstead.pause.error",
                                display_error->code + ": " + display_error->message, 13.0F)))) {
                return status;
            }
            status = add(button(return_id, "Return to Main Menu"));
            if (status) {
                widgets.set_focus(resume_id);
            }
            return status;
        case ApplicationState::session_unloading:
            if (!(status = add(label("heartstead.unloading.title", "Leaving world", 28.0F)))) {
                return status;
            }
            return add(label("heartstead.unloading.phase", "Releasing session resources..."));
        case ApplicationState::load_failure:
        case ApplicationState::connection_failure:
        case ApplicationState::fatal_error: {
            const auto title = state == ApplicationState::fatal_error ? "Fatal error"
                               : state == ApplicationState::connection_failure
                                   ? "Connection failed"
                                   : "World load failed";
            const auto details = display_error.has_value()
                                     ? display_error->code + ": " + display_error->message
                                     : "No diagnostic details were supplied.";
            if (!(status = add(label("heartstead.error.title", title, 28.0F))) ||
                !(status = add(label("heartstead.error.details", details, 14.0F)))) {
                return status;
            }
            if (state != ApplicationState::fatal_error) {
                status = add(button(back_id, "Back to Main Menu"));
                if (status) {
                    widgets.set_focus(back_id);
                }
            }
            return status;
        }
        case ApplicationState::in_game:
        case ApplicationState::shutdown:
            return core::Status::ok();
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Status begin_session_loading() {
        if (loading.valid() || session_runtime.has_value()) {
            return core::Status::failure("heartstead.session_already_pending",
                                         "a runtime session is already active or loading");
        }
        if (!pending_launch.has_value()) {
            return core::Status::failure("heartstead.launch_request_missing",
                                         "session loading requires a prepared launch request");
        }
        loading_stop = std::stop_source{};
        loading_progress = std::make_shared<LoadingProgressState>();
        loading_progress->phase.store(SessionStartupPhase::initializing_content,
                                      std::memory_order_relaxed);
        loading_generation = next_session_generation++;
        const auto stop_token = loading_stop.get_token();
        const auto generation = loading_generation;
        if (services == nullptr || services->jobs() == nullptr) {
            return core::Status::failure("heartstead.loading_jobs_unavailable",
                                         "session loading requires the application job system");
        }
        auto prepared_runtime = application_runtime.create_session_runtime();
        if (!prepared_runtime) {
            return core::Status::failure(prepared_runtime.error().code,
                                         prepared_runtime.error().message);
        }
        auto request = *pending_launch;
        request.ownership_generation = generation;
        request.runtime.gameplay_modules.push_back(
            std::make_shared<animals::WanderingAnimalModule>());
        loading_mode = request.mode;
        const auto mode = request.mode;
        const auto persistence = request.persistence;
        const auto save_slot_id = pending_save_slot;
        const auto world_name = request.world_name;
        const auto progress = loading_progress;
        auto runtime = std::make_shared<GameRuntime>(std::move(prepared_runtime).value());
        auto launch_request = std::make_shared<SessionLaunchRequest>(std::move(request));
        auto completion = std::make_shared<std::promise<SessionLoadResult>>();
        loading = completion->get_future();
        auto submitted = services->jobs()->submit({
            "heartstead.session_loading",
            jobs::JobPriority::high,
            [generation, stop_token, runtime, launch_request, mode, persistence, save_slot_id,
             world_name, progress, completion](const jobs::JobContext&) mutable {
                const auto complete = [&completion](SessionLoadResult result) {
                    completion->set_value(std::move(result));
                    return core::Status::ok();
                };
                try {
                if (stop_token.stop_requested()) {
                    progress->phase.store(SessionStartupPhase::cancelling,
                                          std::memory_order_relaxed);
                    return complete(SessionLoadResult::failure(
                        "heartstead.session_load_cancelled", "session loading was cancelled"));
                }
                auto status = runtime->start_session(
                    std::move(*launch_request), [progress](SessionStartupPhase phase) {
                        progress->phase.store(phase, std::memory_order_relaxed);
                    }, stop_token);
                if (!status) {
                    return complete(
                        SessionLoadResult::failure(status.error().code, status.error().message));
                }
                if (stop_token.stop_requested()) {
                    progress->phase.store(SessionStartupPhase::cancelling,
                                          std::memory_order_relaxed);
                    auto shutdown = runtime->shutdown();
                    if (!shutdown) {
                        return complete(SessionLoadResult::failure(shutdown.error().code,
                                                                   shutdown.error().message));
                    }
                    return complete(SessionLoadResult::failure(
                        "heartstead.session_load_cancelled", "session loading was cancelled"));
                }
                return complete(SessionLoadResult::success(
                    {generation, mode, persistence, save_slot_id, world_name, std::move(*runtime)}));
                } catch (const std::exception& exception) {
                    return complete(SessionLoadResult::failure(
                        "heartstead.session_load_exception",
                        std::string("session loading threw an exception: ") + exception.what()));
                } catch (...) {
                    return complete(SessionLoadResult::failure(
                        "heartstead.session_load_exception",
                        "session loading threw a non-standard exception"));
                }
            },
        });
        if (!submitted) {
            loading = {};
            loading_progress.reset();
            loading_stop = std::stop_source{};
            return core::Status::failure(submitted.error().code, submitted.error().message);
        }
        pending_launch.reset();
        pending_save_slot.clear();
        loading_job = submitted.value();
        return core::Status::ok();
    }

    [[nodiscard]] core::Status transition_after_unload(ApplicationState failure_state,
                                                       std::string reason,
                                                       core::Error error) {
        auto cleanup = unload_session();
        if (!cleanup) {
            error.message += "; session cleanup also failed: " + cleanup.error().code + ": " +
                             cleanup.error().message;
        }
        return states.transition(failure_state, std::move(reason), std::move(error));
    }

    [[nodiscard]] core::Status dismiss_recoverable_error(ApplicationState current_state) {
        if (session_runtime.has_value()) {
            auto cleanup = unload_session();
            if (!cleanup) {
                display_error = cleanup.error();
                return rebuild_ui(current_state);
            }
        }
        menu_navigation.reset();
        auto status = refresh_saves();
        if (!status) {
            display_error = status.error();
            return rebuild_ui(current_state);
        }
        return states.transition(ApplicationState::main_menu, "error message dismissed");
    }

    [[nodiscard]] core::Status activate_loaded_session() {
        if (!session_runtime.has_value() || session_runtime->session() == nullptr) {
            return core::Status::failure("heartstead.loaded_session_missing",
                                         "session activation requires a loaded runtime");
        }
        const auto creating_persistent_world =
            active_persistence == PersistencePolicy::persistent && !active_save_slot.empty() &&
            session_runtime->session()->launch_request().world_source == WorldSourceKind::generated;
        if (creating_persistent_world) {
            auto snapshot = session_runtime->capture_save_snapshot();
            if (!snapshot) {
                return transition_after_unload(ApplicationState::load_failure,
                                               "initial world save failed", snapshot.error());
            }
            auto status = save_catalog.write_snapshot(active_save_slot, snapshot.value(),
                                                      persisted_timestamp_ms());
            if (status) {
                status = save_catalog.rename_slot(active_save_slot, active_world_name);
            }
            if (!status) {
                return transition_after_unload(ApplicationState::load_failure,
                                               "initial world save failed", status.error());
            }
        }
        if (active_persistence == PersistencePolicy::persistent && !active_save_slot.empty()) {
            settings.last_world_slot = active_save_slot;
            const auto settings_status = persist_settings();
            if (!settings_status) {
                menu_message = "World loaded, but recent-world history could not be saved: " +
                               settings_status.error().message;
            }
        }
        if (active_session_mode == SessionMode::remote_multiplayer) {
            settings.recent_servers.erase(std::remove(settings.recent_servers.begin(),
                                                      settings.recent_servers.end(),
                                                      active_world_name),
                                          settings.recent_servers.end());
            settings.recent_servers.insert(settings.recent_servers.begin(), active_world_name);
            if (settings.recent_servers.size() > 16) {
                settings.recent_servers.resize(16);
            }
            const auto settings_status = persist_settings();
            if (!settings_status) {
                menu_message = "Connected, but recent-server history could not be saved: " +
                               settings_status.error().message;
            }
        }
        last_autosave_at_ms = last_runtime_time_ms;
        input_scheduler.reset(session_runtime->session()->fixed_step_tick());
        input_scheduler.set_look_sensitivity(static_cast<double>(settings.mouse_sensitivity) *
                                             12.0);
        input_orientation_initialized = false;
        gameplay_input_enabled = true;
        if (!config.headless && session_runtime->session()->client() != nullptr) {
            auto ui_layer = std::make_unique<GameUiLayer>(
                config.content_report->item_definitions,
                config.content_report->entity_definitions, config.content_report->ui_skin);
            auto ui_status = ui_layer->initialize();
            if (ui_status) {
                auto synchronized =
                    ui_layer->synchronize(*session_runtime->session()->client());
                if (!synchronized) {
                    ui_status = core::Status::failure(synchronized.error().code,
                                                      synchronized.error().message);
                }
            }
            if (ui_status) {
                game_ui = std::move(ui_layer);
            } else {
                core::log(core::LogLevel::warning,
                          "In-game UI disabled: " + ui_status.error().message);
                menu_message = "In-game UI could not start: " + ui_status.error().message;
            }
        }
        if (loading_progress != nullptr) {
            loading_progress->phase.store(SessionStartupPhase::ready, std::memory_order_relaxed);
        }
        return states.transition(ApplicationState::in_game, "session startup completed");
    }

    [[nodiscard]] core::Status take_finished_load(bool enter_game) {
        SessionLoadResult loaded = [&]() {
            try {
                return loading.get();
            } catch (const std::exception& exception) {
                return SessionLoadResult::failure(
                    "heartstead.session_load_exception",
                    std::string("session loading completion failed: ") + exception.what());
            } catch (...) {
                return SessionLoadResult::failure(
                    "heartstead.session_load_exception",
                    "session loading completion failed with a non-standard exception");
            }
        }();
        loading_job.reset();
        if (!loaded) {
            if (!enter_game || loading_stop.stop_requested() ||
                loaded.error().code == "heartstead.session_load_cancelled") {
                return core::Status::ok();
            }
            return states.transition(loading_mode == SessionMode::remote_multiplayer
                                         ? ApplicationState::connection_failure
                                         : ApplicationState::load_failure,
                                     "session startup failed", loaded.error());
        }
        auto completed = std::move(loaded).value();
        auto runtime = std::move(completed.runtime);
        if (!enter_game || loading_stop.stop_requested() ||
            completed.generation != loading_generation) {
            auto status = runtime.shutdown();
            if (!status) {
                return status;
            }
            return core::Status::ok();
        }
        if (runtime.session() == nullptr) {
            return core::Status::failure("heartstead.loaded_session_missing",
                                         "completed session load has no runtime session");
        }
        if (services != nullptr && services->renderer() != nullptr) {
            auto* renderer = services->renderer();
            auto status = runtime.session()->register_cleanup(
                "application renderer session resources",
                [renderer]() { return renderer->clear_session_resources(); });
            if (!status) {
                auto error = status.error();
                auto cleanup = runtime.shutdown();
                if (!cleanup) {
                    error.message += "; loaded runtime cleanup also failed: " +
                                     cleanup.error().code + ": " + cleanup.error().message;
                }
                return states.transition(loading_mode == SessionMode::remote_multiplayer
                                             ? ApplicationState::connection_failure
                                             : ApplicationState::load_failure,
                                         "renderer cleanup registration failed", error);
            }
        }
        session_runtime.emplace(std::move(runtime));
        active_session_mode = completed.mode;
        active_persistence = completed.persistence;
        active_save_slot = completed.save_slot_id;
        active_save_path = session_runtime->session()->launch_request().save_path;
        active_world_name = completed.world_name;
        if (services != nullptr && services->audio() != nullptr) {
            auto* audio = services->audio();
            auto status = audio_presentation.initialize(*audio);
            if (!status) {
                core::log(core::LogLevel::warning,
                          "Session audio presentation disabled: " + status.error().message);
                menu_message = "Audio presentation disabled: " + status.error().message;
            } else {
                audio_presentation_initialized = true;
                status = session_runtime->session()->register_cleanup(
                    "application session audio", [this, audio]() {
                        auto cleanup = audio_presentation.shutdown(*audio);
                        audio_presentation_initialized = false;
                        return cleanup;
                    });
                if (!status) {
                    (void)audio_presentation.shutdown(*audio);
                    audio_presentation_initialized = false;
                    core::log(core::LogLevel::warning,
                              "Session audio cleanup registration failed: " +
                                  status.error().message);
                    menu_message = "Audio presentation disabled: " + status.error().message;
                }
            }
        }
        if (!config.headless && services != nullptr && services->renderer() != nullptr) {
            ModelPresentationSystemConfig presentation_config;
            presentation_config.material_registry = &config.content_report->material_registry;
            auto status = model_presentation.initialize(
                *services->renderer(), config.content_report->visual_definitions,
                config.cooked_asset_root, presentation_config);
            if (!status) {
                core::log(core::LogLevel::warning,
                          "Session model presentation disabled: " + status.error().message);
                menu_message = "Model presentation disabled: " + status.error().message;
            } else {
                model_presentation_initialized = true;
                auto* renderer = services->renderer();
                status = session_runtime->session()->register_cleanup(
                    "application model presentation", [this, renderer]() {
                        auto cleanup = model_presentation.shutdown(*renderer);
                        model_presentation_initialized = false;
                        return cleanup;
                    });
                if (!status) {
                    (void)model_presentation.shutdown(*renderer);
                    model_presentation_initialized = false;
                    core::log(core::LogLevel::warning,
                              "Session model cleanup registration failed: " +
                                  status.error().message);
                    menu_message = "Model presentation disabled: " + status.error().message;
                }
            }
        }
        if (session_runtime->session()->connection_state() == SessionConnectionState::connecting) {
            if (loading_progress != nullptr) {
                loading_progress->phase.store(SessionStartupPhase::connecting_transport,
                                              std::memory_order_relaxed);
            }
            return core::Status::ok();
        }
        return activate_loaded_session();
    }

    [[nodiscard]] core::Status unload_session() {
        auto first_failure = core::Status::ok();
        if (session_runtime.has_value()) {
            auto status = session_runtime->shutdown();
            if (!status && first_failure) {
                first_failure = status;
            }
            if (status) {
                game_ui.reset();
                session_runtime.reset();
                runtime_stats.reset();
                player_camera_frame.reset();
                ++completed_session_count;
            }
        }
        if (!first_failure) {
            return first_failure;
        }
        active_save_slot.clear();
        active_save_path.reset();
        active_world_name.clear();
        active_persistence = PersistencePolicy::ephemeral;
        active_session_mode = SessionMode::local_single_player;
        input_scheduler.reset();
        input_orientation_initialized = false;
        gameplay_input_enabled = true;
        return first_failure;
    }

    [[nodiscard]] core::Status save_active_session() {
        if (!session_runtime.has_value() || active_persistence != PersistencePolicy::persistent) {
            return core::Status::ok();
        }
        auto snapshot = session_runtime->capture_save_snapshot();
        if (!snapshot) {
            return core::Status::failure(snapshot.error().code, snapshot.error().message);
        }
        if (!active_save_slot.empty()) {
            return save_catalog.write_snapshot(active_save_slot, snapshot.value(),
                                               persisted_timestamp_ms());
        }
        if (active_save_path.has_value()) {
            return save::FileSaveDatabase(*active_save_path).write_snapshot(snapshot.value());
        }
        return core::Status::failure("heartstead.persistent_save_path_missing",
                                     "persistent session has no save destination");
    }

    [[nodiscard]] core::Status autosave_if_due() {
        if (!session_runtime.has_value() || active_persistence != PersistencePolicy::persistent ||
            config.autosave_interval_ms <= 0 ||
            last_runtime_time_ms - last_autosave_at_ms < config.autosave_interval_ms) {
            return core::Status::ok();
        }
        auto status = save_active_session();
        if (status) {
            last_autosave_at_ms = last_runtime_time_ms;
            ++periodic_save_count;
        }
        return status;
    }

    [[nodiscard]] core::Result<RuntimeFrameStats> advance_runtime(bool gameplay_input) {
        if (!session_runtime.has_value() || session_runtime->session() == nullptr ||
            frame == nullptr) {
            return core::Result<RuntimeFrameStats>::failure(
                "heartstead.session_missing", "runtime advance requires an active session");
        }
        std::optional<movement::FixedStepPlayerInputFrame> scheduled_input;
        auto* client = session_runtime->session()->client();
        if (states.state() == ApplicationState::in_game && frame->input != nullptr &&
            client != nullptr) {
            const auto* player = client->local_player_snapshot();
            if (player != nullptr && !input_orientation_initialized) {
                input_scheduler.set_orientation(player->state.yaw_centidegrees,
                                                player->state.pitch_centidegrees);
                input_orientation_initialized = true;
            }
            auto scheduled = input_scheduler.advance(*frame->input, frame->delta_microseconds,
                                                      gameplay_input);
            if (!scheduled) {
                return core::Result<RuntimeFrameStats>::failure(scheduled.error().code,
                                                                scheduled.error().message);
            }
            scheduled_input = std::move(scheduled).value();
            for (const auto& player_input : scheduled_input->inputs) {
                if (!client->is_connected() || player == nullptr) {
                    continue;
                }
                auto status = session_runtime->session()->submit_player_input(
                    player_input, frame->now_milliseconds);
                if (!status) {
                    return core::Result<RuntimeFrameStats>::failure(status.error().code,
                                                                    status.error().message);
                }
            }
        }
        auto advanced =
            session_runtime->run_frame({frame->delta_microseconds, frame->now_milliseconds});
        if (!advanced) {
            return advanced;
        }
        if (scheduled_input.has_value() &&
            (advanced.value().fixed_step.step_count != scheduled_input->fixed_step.step_count ||
             advanced.value().fixed_step.first_tick != scheduled_input->fixed_step.first_tick)) {
            return core::Result<RuntimeFrameStats>::failure(
                "heartstead.input_clock_desynchronized",
                "player prediction input diverged from the runtime fixed-step clock");
        }
        return advanced;
    }

    [[nodiscard]] core::Status recover_runtime_failure(const core::Error& error,
                                                       std::string reason) {
        const auto failed_mode = active_session_mode;
        return transition_after_unload(session_mode_is_multiplayer(failed_mode)
                                           ? ApplicationState::connection_failure
                                           : ApplicationState::load_failure,
                                       std::move(reason), error);
    }

    core::Status enter_state(ApplicationState state,
                             const ApplicationTransition& transition) override {
        display_error = transition.error;
        if (services != nullptr && !config.headless) {
            const auto captured =
                application_state_policy(state).cursor_owner == ApplicationCursorOwner::captured;
            auto status = services->set_cursor_capture(captured);
            if (!status) {
                return status;
            }
        }
        auto status = rebuild_ui(state);
        if (!status) {
            return status;
        }
        if (state == ApplicationState::session_loading) {
            return begin_session_loading();
        }
        if (state == ApplicationState::session_unloading) {
            loading_stop.request_stop();
            if (loading_progress != nullptr) {
                loading_progress->phase.store(SessionStartupPhase::cancelling,
                                              std::memory_order_relaxed);
            }
            if (session_runtime.has_value() && session_runtime->session() != nullptr) {
                return session_runtime->session()->request_stop();
            }
        }
        if (state == ApplicationState::in_game && transition.from == ApplicationState::paused &&
            session_runtime.has_value() && session_runtime->session() != nullptr) {
            input_scheduler.reset(session_runtime->session()->fixed_step_tick());
            input_orientation_initialized = false;
        }
        return core::Status::ok();
    }

    core::Status update_state(ApplicationState state, std::uint64_t) override {
        switch (state) {
        case ApplicationState::session_loading:
            if (loading.valid() &&
                loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto status = take_finished_load(true);
                if (!status || states.state() != ApplicationState::session_loading) {
                    return status;
                }
            }
            if (session_runtime.has_value()) {
                auto advanced = advance_runtime(false);
                if (!advanced) {
                    return recover_runtime_failure(advanced.error(),
                                                   "multiplayer connection failed");
                }
                runtime_stats = std::move(advanced).value();
                if (session_runtime->session()->connection_state() ==
                    SessionConnectionState::connected) {
                    return activate_loaded_session();
                }
            }
            {
                const auto phase = loading_progress == nullptr
                                       ? SessionStartupPhase::validating_request
                                       : loading_progress->phase.load(std::memory_order_relaxed);
                if (!rendered_loading_phase.has_value() || *rendered_loading_phase != phase) {
                    return rebuild_ui(ApplicationState::session_loading);
                }
            }
            return core::Status::ok();
        case ApplicationState::in_game:
            if (!session_runtime.has_value() || session_runtime->session() == nullptr ||
                frame == nullptr) {
                return core::Status::failure("heartstead.session_missing",
                                             "in-game state requires an active runtime session");
            }
            {
                auto advanced = advance_runtime(gameplay_input_enabled);
                if (!advanced) {
                    return recover_runtime_failure(advanced.error(), "runtime session failed");
                }
                runtime_stats = std::move(advanced).value();
                if (game_ui != nullptr && session_runtime->session()->client() != nullptr) {
                    auto synchronized =
                        game_ui->synchronize(*session_runtime->session()->client());
                    if (!synchronized) {
                        core::log(core::LogLevel::warning,
                                  "In-game UI synchronization disabled: " +
                                      synchronized.error().message);
                        menu_message = "In-game UI stopped: " + synchronized.error().message;
                        game_ui.reset();
                        gameplay_input_enabled = true;
                    }
                }
                if (runtime_stats->fixed_step.dropped_time_us != 0) {
                    core::log(
                        core::LogLevel::warning,
                        "Runtime fixed-step catch-up dropped " +
                            std::to_string(runtime_stats->fixed_step.dropped_time_us) +
                            " us for session generation " +
                            std::to_string(session_runtime->session()->ownership_generation()));
                }
            }
            break;
        case ApplicationState::paused:
            if (authoritative_simulation_advances(
                    states.policy(), session_mode_is_multiplayer(active_session_mode))) {
                auto advanced = advance_runtime(false);
                if (!advanced) {
                    return recover_runtime_failure(advanced.error(),
                                                   "multiplayer session failed while menu open");
                }
                runtime_stats = std::move(advanced).value();
            }
            break;
        case ApplicationState::session_unloading:
            if (loading.valid()) {
                if (loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                    return core::Status::ok();
                }
                auto status = take_finished_load(false);
                if (!status) {
                    return status;
                }
            }
            {
                const auto failed_mode = active_session_mode;
                auto status = unload_session();
                if (!status) {
                    return states.transition(session_mode_is_multiplayer(failed_mode)
                                                 ? ApplicationState::connection_failure
                                                 : ApplicationState::load_failure,
                                             "session teardown failed", status.error());
                }
            }
            {
                auto status = refresh_saves();
                if (!status) {
                    menu_navigation.reset();
                    return states.transition(ApplicationState::load_failure,
                                             "save list refresh failed after session teardown",
                                             status.error());
                }
            }
            menu_navigation.reset();
            return states.transition(ApplicationState::main_menu, "session resources released");
        case ApplicationState::shutdown:
            if (services != nullptr) {
                services->request_quit();
            }
            break;
        case ApplicationState::boot:
        case ApplicationState::main_menu:
        case ApplicationState::load_failure:
        case ApplicationState::connection_failure:
        case ApplicationState::fatal_error:
            break;
        }
        return core::Status::ok();
    }

    core::Status exit_state(ApplicationState, const ApplicationTransition&) override {
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<save::SaveMetadata> make_metadata(std::uint64_t seed) const {
        return content::save_metadata_from_content_report(*config.content_report, "0.1.0", seed);
    }

    [[nodiscard]] core::Status launch(SessionLaunchRequest request, std::string save_slot = {}) {
        if (pending_launch.has_value() || loading.valid() || session_runtime.has_value()) {
            return core::Status::failure("heartstead.session_already_pending",
                                         "another session is active or loading");
        }
        request.initial_runtime_time_ms =
            std::max<std::int64_t>(0, frame == nullptr ? 0 : frame->now_milliseconds);
        if (config.safe_mode) {
            request.initial_runtime_options.emplace_back("safe-mode");
        }
        pending_launch = std::move(request);
        pending_save_slot = std::move(save_slot);
        return states.transition(ApplicationState::session_loading, "menu launch selected");
    }

    [[nodiscard]] core::Status launch_new_world() {
        auto normalized_name = normalize_world_display_name(new_world_name);
        if (!normalized_name) {
            return core::Status::failure(normalized_name.error().code,
                                         normalized_name.error().message);
        }
        new_world_name = std::move(normalized_name).value();
        const auto slot_id = world_slot_id(new_world_name);
        if (!save::FileSaveSlotCatalog::is_valid_slot_id(slot_id)) {
            return core::Status::failure("heartstead.invalid_world_name",
                                         "world name must contain letters or numbers");
        }
        if (std::ranges::any_of(save_entries, [&slot_id](const save::SaveSlotSummary& entry) {
                return entry.slot_id == slot_id;
            })) {
            return core::Status::failure("heartstead.world_exists",
                                         "a world with that save id already exists");
        }
        auto parsed_seed = parse_world_seed(new_world_seed);
        if (!parsed_seed) {
            return core::Status::failure(parsed_seed.error().code, parsed_seed.error().message);
        }
        const auto seed = parsed_seed.value();
        auto metadata = make_metadata(seed);
        if (!metadata) {
            return core::Status::failure(metadata.error().code, metadata.error().message);
        }
        SessionLaunchRequest request;
        request.mode = SessionMode::local_single_player;
        request.world_source = WorldSourceKind::generated;
        request.persistence = PersistencePolicy::persistent;
        request.world_name = new_world_name;
        request.scenario_id = foundation::scenario_id;
        request.seed = seed;
        request.generator_preset = "temperate_valley";
        request.save_path = save_catalog.root() / slot_id;
        request.metadata = std::move(metadata).value();
        request.runtime.headless = config.headless;
        request.runtime.physics_backend =
            config.headless ? physics::PhysicsBackend::headless : physics::PhysicsBackend::jolt;
        return launch(std::move(request), slot_id);
    }

    [[nodiscard]] core::Status launch_saved_world(std::size_t index, bool host) {
        if (index >= save_entries.size() || !save_is_compatible(save_entries[index])) {
            return core::Status::failure("heartstead.save_not_loadable",
                                         "selected save is corrupt or incompatible");
        }
        const auto& entry = save_entries[index];
        return launch_saved_path(entry.path, entry.metadata.display_name, *entry.snapshot_metadata,
                                 host, entry.slot_id);
    }

    [[nodiscard]] core::Status launch_saved_path(const std::filesystem::path& path,
                                                 std::string world_name,
                                                 const save::SaveMetadata& metadata, bool host,
                                                 std::string slot_id = {}) {
        if (metadata.schema_version != save::current_save_schema_version) {
            return core::Status::failure("heartstead.save_not_loadable",
                                         "save schema is incompatible with this build");
        }
        const auto compatibility = save::SaveCompatibilityChecker::compare(
            metadata, config.content_report->mod_fingerprints);
        if (compatibility.has_errors()) {
            return core::Status::failure("heartstead.save_not_loadable",
                                         "save requires missing or incompatible content");
        }
        SessionLaunchRequest request;
        request.mode = host ? SessionMode::hosted_multiplayer : SessionMode::local_single_player;
        request.world_source = WorldSourceKind::existing_save;
        request.persistence = PersistencePolicy::persistent;
        request.world_name = std::move(world_name);
        request.scenario_id.clear();
        request.save_path = path;
        request.metadata = metadata;
        request.runtime.headless = config.headless;
        request.runtime.physics_backend =
            config.headless ? physics::PhysicsBackend::headless : physics::PhysicsBackend::jolt;
        if (host) {
            auto endpoint = parse_server_endpoint(host_address);
            if (!endpoint) {
                return core::Status::failure(endpoint.error().code, endpoint.error().message);
            }
            request.network_endpoint = std::move(endpoint).value();
        }
        return launch(std::move(request), std::move(slot_id));
    }

    [[nodiscard]] core::Status launch_remote() {
        auto endpoint = parse_server_endpoint(server_address);
        if (!endpoint) {
            return core::Status::failure(endpoint.error().code, endpoint.error().message);
        }
        auto metadata = make_metadata(foundation::world_seed);
        if (!metadata) {
            return core::Status::failure(metadata.error().code, metadata.error().message);
        }
        SessionLaunchRequest request;
        request.mode = SessionMode::remote_multiplayer;
        request.world_source = WorldSourceKind::remote_server;
        request.persistence = PersistencePolicy::ephemeral;
        request.world_name = server_address;
        request.scenario_id = foundation::scenario_id;
        request.network_endpoint = endpoint.value();
        request.metadata = std::move(metadata).value();
        request.runtime.headless = config.headless;
        return launch(std::move(request));
    }

    [[nodiscard]] core::Status launch_developer_world(std::size_t index) {
        if (index >= developer_worlds.entries().size()) {
            return core::Status::failure("heartstead.developer_world_not_selected",
                                         "select a developer world before launching");
        }
        const auto& definition = developer_worlds.entries()[index];
        auto metadata = make_metadata(definition.world_seed.value_or(foundation::world_seed));
        if (!metadata) {
            return core::Status::failure(metadata.error().code, metadata.error().message);
        }
        auto request = developer_worlds.make_launch_request(
            definition.prototype_id.value(), std::move(metadata).value(), config.headless);
        if (!request) {
            return core::Status::failure(request.error().code, request.error().message);
        }
        return launch(std::move(request).value());
    }

    [[nodiscard]] core::Status launch_developer_world(std::string_view scenario_id,
                                                      std::optional<std::uint64_t> seed) {
        const auto* definition = developer_worlds.find(scenario_id);
        if (definition == nullptr) {
            return core::Status::failure("heartstead.scenario_not_found",
                                         "no developer world is registered as '" +
                                             std::string(scenario_id) + "'");
        }
        auto metadata =
            make_metadata(seed.value_or(definition->world_seed.value_or(foundation::world_seed)));
        if (!metadata) {
            return core::Status::failure(metadata.error().code, metadata.error().message);
        }
        auto request = developer_worlds.make_launch_request(
            definition->prototype_id.value(), std::move(metadata).value(), config.headless);
        if (!request) {
            return core::Status::failure(request.error().code, request.error().message);
        }
        if (seed.has_value()) {
            request.value().seed = seed;
            request.value().metadata.world_seed = *seed;
        }
        return launch(std::move(request).value());
    }

    [[nodiscard]] core::Status launch_world_target(std::string_view target, bool host) {
        for (std::size_t index = 0; index < save_entries.size(); ++index) {
            if (save_entries[index].slot_id == target) {
                return launch_saved_world(index, host);
            }
        }
        const auto path = std::filesystem::path(target);
        auto snapshot = save::FileSaveDatabase(path).read_snapshot();
        if (!snapshot) {
            return core::Status::failure(snapshot.error().code, snapshot.error().message);
        }
        auto name = path.filename().string();
        if (name.empty()) {
            name = "External World";
        }
        return launch_saved_path(path, std::move(name), snapshot.value().metadata, host);
    }

    [[nodiscard]] core::Status apply_initial_launch() {
        if (!config.initial_launch.has_value()) {
            return core::Status::ok();
        }
        auto directive = std::move(*config.initial_launch);
        config.initial_launch.reset();
        core::Status status = core::Status::ok();
        switch (directive.kind) {
        case InitialLaunchKind::scenario:
            status = launch_developer_world(directive.target, directive.seed);
            break;
        case InitialLaunchKind::world:
            status = launch_world_target(directive.target, false);
            break;
        case InitialLaunchKind::new_world:
            new_world_name = directive.target;
            if (directive.seed.has_value()) {
                new_world_seed = std::to_string(*directive.seed);
            }
            status = launch_new_world();
            break;
        case InitialLaunchKind::connect:
            server_address = directive.target;
            status = launch_remote();
            break;
        case InitialLaunchKind::host:
            status = launch_world_target(directive.target, true);
            break;
        }
        if (status) {
            return status;
        }
        return states.transition(directive.kind == InitialLaunchKind::connect
                                     ? ApplicationState::connection_failure
                                     : ApplicationState::load_failure,
                                 "command-line launch rejected", status.error());
    }

    [[nodiscard]] std::optional<core::PrototypeId> selected_placement_voxel() const {
        if (game_ui == nullptr || game_ui->selected_hotbar_item() == nullptr) {
            return std::nullopt;
        }
        const auto& selected_item = game_ui->selected_hotbar_item()->prototype_id;
        const auto definitions = config.content_report->voxel_palette.definitions();
        const auto voxel = std::ranges::find_if(definitions, [&selected_item](const auto* value) {
            return value != nullptr && value->interaction.break_resource_item.has_value() &&
                   *value->interaction.break_resource_item == selected_item;
        });
        return voxel == definitions.end() ? std::nullopt
                                          : std::optional<core::PrototypeId>{
                                                (*voxel)->prototype_id};
    }

    [[nodiscard]] core::Status process_input(const GameApplicationFrame& current_frame) {
        if (current_frame.input == nullptr) {
            return core::Status::ok();
        }
        const auto state = states.state();
        if (state != ApplicationState::in_game &&
            key_pressed(*current_frame.input, platform::KeyCode::f3)) {
            diagnostics_visible = !diagnostics_visible;
        }
        if (state == ApplicationState::in_game && session_runtime.has_value() &&
            session_runtime->session() != nullptr) {
            ui::UiInputConsumption consumed;
            if (game_ui != nullptr) {
                auto processed = game_ui->process_input(*current_frame.input,
                                                        *session_runtime->session(),
                                                        current_frame.now_milliseconds);
                if (!processed) {
                    core::log(core::LogLevel::warning,
                              "In-game UI input disabled: " + processed.error().message);
                    menu_message = "In-game UI stopped: " + processed.error().message;
                    game_ui.reset();
                    gameplay_input_enabled = true;
                } else {
                    consumed = processed.value().consumed;
                    gameplay_input_enabled = !game_ui->blocks_gameplay();
                    if ((processed.value().inventory_toggled || processed.value().map_toggled) &&
                        services != nullptr) {
                        auto status = services->set_cursor_capture(gameplay_input_enabled);
                        if (!status) {
                            return status;
                        }
                    }
                }
            } else {
                gameplay_input_enabled = true;
            }
            actions.set_context(gameplay_input_enabled ? input::InputContext::gameplay
                                                       : input::InputContext::inventory);
            const auto action_frame = actions.evaluate(*current_frame.input);
            if (!consumed.keyboard &&
                action_frame[input::InputAction::toggle_debug].pressed) {
                diagnostics_visible = !diagnostics_visible;
            }
            if (!consumed.keyboard &&
                action_frame[input::InputAction::toggle_camera].pressed) {
                camera_perspective =
                    camera_perspective == movement::PlayerCameraPerspective::third_person
                        ? movement::PlayerCameraPerspective::first_person
                        : movement::PlayerCameraPerspective::third_person;
                settings.first_person_camera =
                    camera_perspective == movement::PlayerCameraPerspective::first_person;
                settings_persist_pending = true;
                settings_persist_after_ms = current_frame.now_milliseconds + 500;
            }
            if (!consumed.keyboard &&
                action_frame[input::InputAction::close_or_pause].pressed) {
                return states.transition(ApplicationState::paused, "pause requested");
            }
            const auto remove = !consumed.pointer && gameplay_input_enabled &&
                                action_frame[input::InputAction::primary_action].pressed;
            const auto place = !consumed.pointer && gameplay_input_enabled &&
                               action_frame[input::InputAction::secondary_action].pressed;
            if (remove == place || !player_camera_frame.has_value() ||
                session_runtime->session()->client() == nullptr) {
                return core::Status::ok();
            }
            auto* client = session_runtime->session()->client();
            const auto* player = client->local_player_snapshot();
            if (player != nullptr) {
                const auto camera_from_player =
                    player_camera_frame->position.relative_to(player->state.position.anchor) -
                    player->state.position.local_offset;
                const auto distance =
                    interaction::maximum_voxel_interaction_reach + math::length(camera_from_player);
                auto hit = interaction::raycast_voxels(
                    client->world().chunks(),
                    {player_camera_frame->position, player_camera_frame->forward, distance},
                    &config.content_report->voxel_palette);
                if (!hit) {
                    return core::Status::failure(hit.error().code, hit.error().message);
                }
                if (hit.value().hit.has_value()) {
                    auto reachable = interaction::validate_voxel_interaction_reach(
                        hit.value().hit->block, player->state);
                    if (!reachable && reachable.error().code != "voxel_command.out_of_reach") {
                        return reachable;
                    }
                    if (reachable) {
                        if (remove) {
                            return session_runtime->session()->submit_remove_voxel(
                                {hit.value().hit->block}, current_frame.now_milliseconds);
                        }
                        const auto voxel = selected_placement_voxel();
                        if (!voxel.has_value()) {
                            return core::Status::ok();
                        }
                        return session_runtime->session()->submit_place_voxel(
                            {hit.value().hit->adjacent_block, *voxel},
                            current_frame.now_milliseconds);
                    }
                }
            }
            return core::Status::ok();
        }
        if (config.headless || state == ApplicationState::in_game ||
            state == ApplicationState::session_unloading || state == ApplicationState::shutdown) {
            return core::Status::ok();
        }
        auto status = widgets.layout({static_cast<float>(current_frame.extent.width),
                                      static_cast<float>(current_frame.extent.height)},
                                     settings.ui_scale);
        if (!status) {
            return status;
        }
        const auto routed =
            widgets.route_input(ui::UiInputFrame::from_platform(*current_frame.input));
        for (const auto& event : routed.events) {
            if (event.kind == ui::UiEventKind::text_changed) {
                if (event.target == world_name_id)
                    new_world_name = event.text;
                else if (event.target == world_seed_id)
                    new_world_seed = event.text;
                else if (event.target == rename_world_name_id)
                    rename_world_name = event.text;
                else if (event.target == address_id)
                    server_address = event.text;
                else if (event.target == host_address_id)
                    host_address = event.text;
                else if (event.target == developer_search_id) {
                    developer_search = event.text;
                    selected_developer_world.reset();
                    return rebuild_ui(ApplicationState::main_menu);
                }
                continue;
            }
            if (event.kind == ui::UiEventKind::value_changed ||
                event.kind == ui::UiEventKind::value_committed ||
                event.kind == ui::UiEventKind::toggled) {
                if (event.target == master_volume_id)
                    settings.master_volume = event.value;
                else if (event.target == music_volume_id)
                    settings.music_volume = event.value;
                else if (event.target == effects_volume_id)
                    settings.effects_volume = event.value;
                else if (event.target == mouse_sensitivity_id)
                    settings.mouse_sensitivity = event.value;
                else if (event.target == ui_scale_id)
                    settings.ui_scale = event.value;
                else if (event.target == contrast_id)
                    settings.ui_contrast = event.value;
                else if (event.target == saturation_id)
                    settings.ui_saturation = event.value;
                else if (event.target == windowed_id)
                    settings.windowed = event.checked;
                else if (event.target == vsync_id)
                    settings.vsync = event.checked;
                status = apply_settings();
                if (status && event.kind != ui::UiEventKind::value_changed)
                    status = persist_settings();
                if (!status) {
                    menu_message = status.error().code + ": " + status.error().message;
                    return rebuild_ui(ApplicationState::main_menu);
                }
                continue;
            }
            if (event.kind == ui::UiEventKind::cancelled) {
                if (state == ApplicationState::session_loading) {
                    return states.transition(ApplicationState::session_unloading,
                                             "session loading cancelled");
                }
                if (state == ApplicationState::paused) {
                    return states.transition(ApplicationState::in_game, "pause dismissed");
                }
                if (state == ApplicationState::main_menu) {
                    if (menu_navigation.back()) {
                        pending_delete_slot.reset();
                        menu_message.clear();
                        return rebuild_ui(ApplicationState::main_menu);
                    }
                    return states.transition(ApplicationState::shutdown, "menu back requested");
                }
                if (state == ApplicationState::load_failure ||
                    state == ApplicationState::connection_failure) {
                    return dismiss_recoverable_error(state);
                }
            }
            if (event.kind != ui::UiEventKind::clicked) {
                continue;
            }
            if (event.target == quit_id) {
                return states.transition(ApplicationState::shutdown, "quit selected");
            }
            if (event.target == resume_id) {
                return states.transition(ApplicationState::in_game, "resume selected");
            }
            if (event.target == return_id || event.target == cancel_id) {
                if (event.target == return_id) {
                    status = save_active_session();
                    if (!status) {
                        display_error = status.error();
                        return rebuild_ui(ApplicationState::paused);
                    }
                }
                return states.transition(ApplicationState::session_unloading,
                                         event.target == cancel_id ? "session loading cancelled"
                                                                   : "return to menu selected");
            }
            if (event.target == back_id) {
                return dismiss_recoverable_error(state);
            }
            if (state != ApplicationState::main_menu) {
                continue;
            }
            if (event.target == menu_back_id || event.target == delete_cancel_id) {
                pending_delete_slot.reset();
                pending_delete_name.clear();
                if (!menu_navigation.back()) {
                    return core::Status::failure("heartstead.menu_at_root",
                                                 "the root menu has no parent screen");
                }
                menu_message.clear();
                return rebuild_ui(ApplicationState::main_menu);
            }
            if (event.target == new_world_id)
                return show_menu(MainMenuScreen::new_world);
            if (event.target == load_world_id) {
                status = refresh_saves();
                return !status ? status : show_menu(MainMenuScreen::load_world);
            }
            if (event.target == multiplayer_id)
                return show_menu(MainMenuScreen::multiplayer);
            if (event.target == developer_worlds_id)
                return show_menu(MainMenuScreen::developer_worlds);
            if (event.target == options_id)
                return show_menu(MainMenuScreen::options);
            if (event.target == developer_category_id) {
                developer_category = next_scenario_category(developer_category);
                selected_developer_world.reset();
                return rebuild_ui(ApplicationState::main_menu);
            }
            if (event.target == quality_id) {
                settings.rendering_quality = next_quality(settings.rendering_quality);
                status = persist_settings();
                return !status ? status : rebuild_ui(ApplicationState::main_menu);
            }
            if (event.target == color_vision_id) {
                settings.color_vision_mode = next_color_vision(settings.color_vision_mode);
                status = apply_settings();
                if (status)
                    status = persist_settings();
                return !status ? status : rebuild_ui(ApplicationState::main_menu);
            }
            if (event.target == continue_id) {
                const auto index = continue_index();
                status = index.has_value()
                             ? launch_saved_world(*index, false)
                             : core::Status::failure("heartstead.no_continue_world",
                                                     "no compatible recent world exists");
            } else if (event.target == create_world_id) {
                status = launch_new_world();
            } else if (event.target == join_id) {
                status = launch_remote();
            } else if (event.target == load_selected_id || event.target == host_selected_id) {
                status = selected_save.has_value()
                             ? launch_saved_world(*selected_save, event.target == host_selected_id)
                             : core::Status::failure("heartstead.save_not_selected",
                                                     "select a save first");
            } else if (event.target == rename_selected_id && selected_save.has_value()) {
                rename_world_name = save_entries[*selected_save].metadata.display_name;
                return show_menu(MainMenuScreen::rename_world);
            } else if (event.target == rename_confirm_id && selected_save.has_value()) {
                auto normalized = normalize_world_display_name(rename_world_name);
                if (!normalized) {
                    status = core::Status::failure(normalized.error().code,
                                                   normalized.error().message);
                } else {
                    const auto slot_id = save_entries[*selected_save].slot_id;
                    status = save_catalog.rename_slot(slot_id, normalized.value());
                    if (status)
                        status = refresh_saves(slot_id);
                    if (status) {
                        (void)menu_navigation.back();
                        menu_message = "World renamed.";
                        return rebuild_ui(ApplicationState::main_menu);
                    }
                }
            } else if (event.target == duplicate_selected_id && selected_save.has_value()) {
                const auto& entry = save_entries[*selected_save];
                auto destination = entry.slot_id + "_copy";
                std::uint32_t suffix = 2;
                while (std::ranges::any_of(save_entries, [&destination](const auto& candidate) {
                    return candidate.slot_id == destination;
                }))
                    destination = entry.slot_id + "_copy_" + std::to_string(suffix++);
                const auto saved_at = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(1, current_frame.wall_clock_milliseconds));
                status = save_catalog.duplicate_slot(
                    entry.slot_id, destination, entry.metadata.display_name + " Copy", saved_at);
                if (status)
                    status = refresh_saves(destination);
                if (status) {
                    menu_message = "World copy created.";
                    return rebuild_ui(ApplicationState::main_menu);
                }
            } else if (event.target == migrate_selected_id && selected_save.has_value()) {
                const auto& entry = save_entries[*selected_save];
                auto destination = entry.slot_id + "_migrated";
                std::uint32_t suffix = 2;
                while (std::ranges::any_of(save_entries, [&destination](const auto& candidate) {
                    return candidate.slot_id == destination;
                }))
                    destination = entry.slot_id + "_migrated_" + std::to_string(suffix++);
                const auto saved_at = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(1, current_frame.wall_clock_milliseconds));
                status = save_catalog.duplicate_slot(
                    entry.slot_id, destination, entry.metadata.display_name + " (Migrated)",
                    saved_at);
                if (status) {
                    save::SaveMigrationRegistry migrations;
                    status = migrations.register_migration({
                        "0001-current-save-schema", 1, save::current_save_schema_version,
                        "Upgrade legacy save metadata to the current schema",
                        [](save::SaveSnapshot&) { return core::Status::ok(); }});
                    if (status) {
                        auto migrated = save::FileSaveDatabase(save_catalog.root() / destination)
                                            .migrate_to_schema(
                                                migrations, save::current_save_schema_version);
                        if (!migrated)
                            status = core::Status::failure(migrated.error().code,
                                                           migrated.error().message);
                    }
                    if (!status) {
                        const auto cleanup = save_catalog.delete_slot(destination);
                        if (!cleanup)
                            menu_message = "Migration failed and its temporary copy could not be "
                                           "removed: " +
                                           cleanup.error().message;
                    }
                }
                if (status)
                    status = refresh_saves(destination);
                if (status) {
                    menu_message = "Migrated copy created; the original was left unchanged.";
                    return rebuild_ui(ApplicationState::main_menu);
                }
            } else if (event.target == delete_selected_id && selected_save.has_value()) {
                pending_delete_slot = save_entries[*selected_save].slot_id;
                pending_delete_name = save_entries[*selected_save].metadata.display_name;
                return show_menu(MainMenuScreen::delete_confirmation);
            } else if (event.target == delete_confirm_id && pending_delete_slot.has_value()) {
                const auto previous_index = selected_save.value_or(0);
                status = save_catalog.delete_slot(*pending_delete_slot);
                pending_delete_slot.reset();
                pending_delete_name.clear();
                if (status)
                    status = refresh_saves();
                if (status) {
                    if (!save_entries.empty())
                        selected_save = std::min(previous_index, save_entries.size() - 1);
                    (void)menu_navigation.back();
                    menu_message = "World deleted.";
                    return rebuild_ui(ApplicationState::main_menu);
                }
            } else if (event.target == refresh_worlds_id) {
                status = refresh_saves();
                if (status) {
                    menu_message = "Save list refreshed.";
                    return rebuild_ui(ApplicationState::main_menu);
                }
            } else if (event.target == copy_save_path_id && selected_save.has_value()) {
                status = services->set_clipboard_text(save_entries[*selected_save].path.string());
                if (status) {
                    menu_message = "Save location copied to clipboard.";
                    return rebuild_ui(ApplicationState::main_menu);
                }
            } else if (event.target == developer_launch_id) {
                status = selected_developer_world.has_value()
                             ? launch_developer_world(*selected_developer_world)
                             : core::Status::failure("heartstead.developer_world_not_selected",
                                                     "select a developer world first");
            } else {
                for (std::size_t index = 0; index < save_entries.size(); ++index) {
                    if (event.target ==
                        ui::widget_id("heartstead.save." + save_entries[index].slot_id)) {
                        selected_save = index;
                        return rebuild_ui(ApplicationState::main_menu);
                    }
                }
                const auto& developer_entries = developer_worlds.entries();
                for (std::size_t index = 0; index < developer_entries.size(); ++index) {
                    if (event.target ==
                        ui::widget_id("heartstead.developer." +
                                      developer_entries[index].prototype_id.value())) {
                        selected_developer_world = index;
                        return rebuild_ui(ApplicationState::main_menu);
                    }
                }
                for (std::size_t index = 0; index < settings.recent_servers.size(); ++index) {
                    if (event.target ==
                        ui::widget_id("heartstead.recent_server." + std::to_string(index))) {
                        server_address = settings.recent_servers[index];
                        return rebuild_ui(ApplicationState::main_menu);
                    }
                }
                continue;
            }
            if (!status) {
                menu_message = status.error().code + ": " + status.error().message;
                return rebuild_ui(ApplicationState::main_menu);
            }
            return status;
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<renderer::RenderCamera>
    prepare_camera_and_world(const GameApplicationFrame& current_frame) {
        renderer::RenderCamera camera;
        player_camera_frame.reset();
        auto status =
            camera.set_aspect_ratio(static_cast<float>(std::max(1U, current_frame.extent.width)) /
                                    static_cast<float>(std::max(1U, current_frame.extent.height)));
        if (!status) {
            return core::Result<renderer::RenderCamera>::failure(status.error().code,
                                                                 status.error().message);
        }
        if (config.headless) {
            return core::Result<renderer::RenderCamera>::success(camera);
        }
        if (!states.policy().world_rendering || !session_runtime.has_value() ||
            session_runtime->session() == nullptr ||
            session_runtime->session()->client() == nullptr) {
            return core::Result<renderer::RenderCamera>::success(camera);
        }
        auto* client = session_runtime->session()->client();
        if (const auto* player = client->local_player_snapshot(); player != nullptr) {
            const movement::PlayerCameraCollisionContext collision{
                client->world().chunks(), config.content_report->voxel_palette};
            auto camera_frame = camera_rig.evaluate(
                player->state, camera_perspective, current_frame.extent.width,
                current_frame.extent.height, &collision, current_frame.delta_seconds());
            if (!camera_frame) {
                return core::Result<renderer::RenderCamera>::failure(camera_frame.error().code,
                                                                     camera_frame.error().message);
            }
            player_camera_frame = camera_frame.value();
            camera = camera_from_frame(camera_frame.value());
        }
        if (services != nullptr && services->renderer() != nullptr) {
            status = services->renderer()->synchronize_chunks(client->world(), camera);
            if (!status) {
                return core::Result<renderer::RenderCamera>::failure(status.error().code,
                                                                     status.error().message);
            }
            if (model_presentation_initialized) {
                auto snapshot = session_runtime->capture_render_snapshot();
                if (!snapshot) {
                    return core::Result<renderer::RenderCamera>::failure(snapshot.error().code,
                                                                         snapshot.error().message);
                }
                if (const auto* player = client->local_player_snapshot();
                    player != nullptr && player_camera_frame.has_value()) {
                    for (auto& object : snapshot.value().objects) {
                        if (object.source_net_id == player->player_net_id) {
                            object.visible = player_camera_frame->body.local_body_visible;
                            break;
                        }
                    }
                }
                auto synchronized = model_presentation.synchronize(*services->renderer(),
                                                                   snapshot.value(), &camera);
                if (!synchronized) {
                    return core::Result<renderer::RenderCamera>::failure(
                        synchronized.error().code, synchronized.error().message);
                }
            }
        }
        return core::Result<renderer::RenderCamera>::success(camera);
    }

    [[nodiscard]] core::Status update_audio(const GameApplicationFrame& current_frame) {
        if (services == nullptr || services->audio() == nullptr) {
            return core::Status::ok();
        }
        if (audio_presentation_initialized && player_camera_frame.has_value() &&
            session_runtime.has_value() && session_runtime->session() != nullptr &&
            session_runtime->session()->client() != nullptr &&
            !(states.state() == ApplicationState::paused &&
              !session_mode_is_multiplayer(active_session_mode))) {
            const auto* player = session_runtime->session()->client()->local_player_snapshot();
            if (player != nullptr) {
                return audio_presentation.update(
                    *services->audio(), player->state, *player_camera_frame,
                    session_runtime->session()->client()->world().chunks(),
                    config.content_report->voxel_palette, current_frame.delta_seconds());
            }
        }
        return services->audio()->update(current_frame.delta_seconds());
    }

    [[nodiscard]] core::Status paint_ui(const GameApplicationFrame& current_frame) {
        if (config.headless || states.state() == ApplicationState::shutdown ||
            services == nullptr || services->renderer() == nullptr ||
            services->renderer()->ui_renderer() == nullptr) {
            return core::Status::ok();
        }
        auto* ui_renderer = services->renderer()->ui_renderer();
        auto status = core::Status::ok();
        if (states.state() == ApplicationState::in_game && game_ui != nullptr) {
            auto painted = game_ui->paint(*ui_renderer, current_frame.extent, settings.ui_scale);
            if (!painted) {
                core::log(core::LogLevel::warning,
                          "In-game UI painting disabled: " + painted.error().message);
                menu_message = "In-game UI stopped: " + painted.error().message;
                game_ui.reset();
            } else {
                services->renderer()->set_ui_widget_stats(
                    game_ui->stats().layout_ms, game_ui->stats().paint_ms,
                    game_ui->stats().layout.widget_count);
            }
        } else if (states.state() != ApplicationState::in_game) {
            status = widgets.layout({static_cast<float>(current_frame.extent.width),
                                     static_cast<float>(current_frame.extent.height)},
                                    settings.ui_scale);
            if (!status) {
                return status;
            }
            auto painted = widgets.paint(*ui_renderer);
            if (!painted) {
                return core::Status::failure(painted.error().code, painted.error().message);
            }
            services->renderer()->set_ui_widget_stats(0.0, 0.0,
                                                      widgets.layout_stats().widget_count);
        }
        if (diagnostics_visible) {
            renderer::UiQuadDesc panel;
            panel.minimum_pixels = {12.0F, 12.0F};
            panel.maximum_pixels = {
                std::min(660.0F, static_cast<float>(current_frame.extent.width) - 12.0F),
                std::min(230.0F, static_cast<float>(current_frame.extent.height) - 12.0F)};
            panel.color = {0.015F, 0.025F, 0.04F, 0.90F};
            status = ui_renderer->submit_quad(panel);
            if (status) {
                status =
                    ui_renderer->submit_text({{20.0F, 20.0F},
                                              format_runtime_diagnostics(diagnostics_snapshot()),
                                              12.0F,
                                              {0.82F, 0.92F, 1.0F, 1.0F}});
            }
        }
        return status;
    }
};

HeartsteadApplicationMode::HeartsteadApplicationMode(HeartsteadApplicationModeConfig config)
    : implementation_(std::make_unique<Impl>(std::move(config))) {}

HeartsteadApplicationMode::~HeartsteadApplicationMode() = default;

core::Status HeartsteadApplicationMode::initialize(GameApplicationServices& services) {
    auto& state = *implementation_;
    if (state.config.autosave_interval_ms <= 0) {
        return core::Status::failure("heartstead.invalid_autosave_interval",
                                     "autosave interval must be positive");
    }
    if (state.config.content_report == nullptr || state.config.content_report->has_errors()) {
        return core::Status::failure("heartstead.invalid_content",
                                     "Heartstead requires validated content at application boot");
    }
    state.services = &services;
    auto runtime = GameRuntime::initialize(GameRuntimeConfig{}, *state.config.content_report);
    if (!runtime) {
        return core::Status::failure(runtime.error().code, runtime.error().message);
    }
    state.application_runtime = std::move(runtime).value();
    auto audio = state.application_runtime.create_audio_system(
        state.config.headless ? audio::AudioBackend::null_backend : audio::AudioBackend::miniaudio,
        {}, state.config.headless, state.config.cooked_assets);
    if (!audio) {
        return core::Status::failure(audio.error().code, audio.error().message);
    }
    auto status = services.install_audio_system(std::move(audio).value());
    if (!status) {
        return status;
    }
    auto developer_worlds =
        DeveloperWorldRegistry::create(state.config.content_report->scenario_definitions);
    if (!developer_worlds) {
        return core::Status::failure(developer_worlds.error().code,
                                     developer_worlds.error().message);
    }
    state.developer_worlds = std::move(developer_worlds).value();
    status = state.refresh_saves();
    if (!status) {
        return status;
    }
    status = state.apply_settings();
    if (!status) {
        return status;
    }
    status = state.states.start();
    if (!status) {
        return status;
    }
    status =
        state.states.transition(ApplicationState::main_menu, "application services initialized");
    if (!status) {
        return status;
    }
    status = state.apply_initial_launch();
    if (!status) {
        return status;
    }
    state.initialized = true;
    return core::Status::ok();
}

core::Result<GameApplicationFrameOutput>
HeartsteadApplicationMode::update(GameApplicationServices& services,
                                  const GameApplicationFrame& frame) {
    auto& state = *implementation_;
    state.services = &services;
    state.frame = &frame;
    struct FramePointerReset {
        const GameApplicationFrame*& pointer;
        ~FramePointerReset() {
            pointer = nullptr;
        }
    } frame_pointer_reset{state.frame};
    state.last_runtime_time_ms = frame.now_milliseconds;
    state.last_wall_clock_ms = frame.wall_clock_milliseconds;
    ++state.frame_count;
    if (!frame.headless && frame.extent.is_valid()) {
        const auto width = std::clamp(frame.extent.width, 640U, 16'384U);
        const auto height = std::clamp(frame.extent.height, 360U, 16'384U);
        if (state.settings.window_width != width || state.settings.window_height != height) {
            state.settings.window_width = width;
            state.settings.window_height = height;
            state.settings_persist_pending = true;
            state.settings_persist_after_ms = frame.now_milliseconds + 500;
        }
    }
    if (state.settings_persist_pending &&
        frame.now_milliseconds >= state.settings_persist_after_ms) {
        auto persisted = state.persist_settings();
        if (!persisted) {
            state.display_error = persisted.error();
            state.settings_persist_after_ms = frame.now_milliseconds + 5'000;
        }
    }
    auto status = state.process_input(frame);
    if (status) {
        status = state.states.update(frame.delta_microseconds);
    }
    if (status && state.states.state() == ApplicationState::in_game) {
        status = state.autosave_if_due();
        if (!status) {
            state.display_error = status.error();
            state.menu_message = "Autosave failed: " + status.error().message;
            status = core::Status::ok();
        }
    }
    if (!status) {
        return core::Result<GameApplicationFrameOutput>::failure(status.error().code,
                                                                 status.error().message);
    }
    auto camera = state.prepare_camera_and_world(frame);
    if (!camera) {
        core::log(core::LogLevel::warning,
                  "Session presentation degraded: " + camera.error().message);
        state.menu_message = "Visual presentation degraded: " + camera.error().message;
        renderer::RenderCamera fallback;
        status = fallback.set_aspect_ratio(static_cast<float>(std::max(1U, frame.extent.width)) /
                                           static_cast<float>(std::max(1U, frame.extent.height)));
        if (!status) {
            return core::Result<GameApplicationFrameOutput>::failure(status.error().code,
                                                                     status.error().message);
        }
        camera = core::Result<renderer::RenderCamera>::success(std::move(fallback));
    }
    status = state.update_audio(frame);
    if (!status && state.session_runtime.has_value()) {
        const auto error = status.error();
        core::log(core::LogLevel::warning,
                  "Session audio presentation disabled: " + error.message);
        state.menu_message = "Audio presentation disabled: " + error.message;
        if (state.audio_presentation_initialized && services.audio() != nullptr) {
            (void)state.audio_presentation.shutdown(*services.audio());
            state.audio_presentation_initialized = false;
        }
        status = services.audio() == nullptr
                     ? core::Status::ok()
                     : services.audio()->update(frame.delta_seconds());
    }
    if (status) {
        status = state.paint_ui(frame);
    }
    if (!status) {
        return core::Result<GameApplicationFrameOutput>::failure(status.error().code,
                                                                 status.error().message);
    }
    if (state.config.headless) {
        return core::Result<GameApplicationFrameOutput>::success({});
    }
    GameApplicationFrameOutput output;
    const auto interpolation =
        state.runtime_stats.has_value()
            ? static_cast<float>(state.runtime_stats->fixed_step.interpolation_alpha)
            : 1.0F;
    output.render =
        renderer::RenderFrameInput{camera.value(), interpolation, frame.delta_seconds()};
    return core::Result<GameApplicationFrameOutput>::success(std::move(output));
}

core::Status HeartsteadApplicationMode::shutdown(GameApplicationServices&) {
    auto& state = *implementation_;
    auto first_failure = state.save_active_session();
    auto status = state.unload_session();
    if (!status && first_failure) {
        first_failure = status;
    }
    if (state.loading.valid()) {
        state.loading_stop.request_stop();
        if (state.loading_progress != nullptr) {
            state.loading_progress->phase.store(SessionStartupPhase::cancelling,
                                                std::memory_order_relaxed);
        }
        if (state.loading.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
            status = state.take_finished_load(false);
            if (!status && first_failure) {
                first_failure = status;
            }
        } else if (first_failure) {
            first_failure = core::Status::failure(
                "heartstead.session_cancel_timeout",
                "session loading did not acknowledge cancellation within two seconds");
        }
    }
    status = state.application_runtime.shutdown();
    if (!status && first_failure) {
        first_failure = status;
    }
    state.services = nullptr;
    state.frame = nullptr;
    state.initialized = false;
    return first_failure;
}

std::string HeartsteadApplicationMode::summary() const {
    const auto& state = *implementation_;
    return "heartstead application: state=" +
           std::string(application_state_name(state.states.state())) +
           " frames=" + std::to_string(state.frame_count) +
           " completed_sessions=" + std::to_string(state.completed_session_count) +
           " autosaves=" + std::to_string(state.periodic_save_count);
}

} // namespace heartstead::game
