#include "game/application/heartstead_application_mode.hpp"

#include "engine/assets/cooked_asset_store.hpp"
#include "engine/audio/audio_system.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/input/input_action.hpp"
#include "engine/movement/player_camera.hpp"
#include "engine/ui/widget_tree.hpp"
#include "game/application/application_state.hpp"
#include "game/features/animals/wandering_animal_module.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/runtime/game_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <utility>

namespace heartstead::game {

namespace {

const auto root_id = ui::widget_id("heartstead.shell.root");
const auto panel_id = ui::widget_id("heartstead.shell.panel");
const auto start_id = ui::widget_id("heartstead.menu.start");
const auto quit_id = ui::widget_id("heartstead.menu.quit");
const auto resume_id = ui::widget_id("heartstead.pause.resume");
const auto return_id = ui::widget_id("heartstead.pause.return");
const auto cancel_id = ui::widget_id("heartstead.loading.cancel");
const auto back_id = ui::widget_id("heartstead.error.back");

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
    panel.kind = ui::WidgetKind::panel;
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

[[nodiscard]] ui::WidgetDesc button(ui::WidgetId id, std::string text, std::string tooltip = {}) {
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
    result.focusable = true;
    result.pointer_events = true;
    result.color = {0.42F, 0.23F, 0.075F, 1.0F};
    return result;
}

[[nodiscard]] bool key_pressed(const platform::WindowInputSnapshot& input,
                               platform::KeyCode key) noexcept {
    return std::ranges::find(input.pressed_keys, key) != input.pressed_keys.end();
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
    GameRuntime runtime;
};

using SessionLoadResult = core::Result<LoadedSession>;

} // namespace

struct HeartsteadApplicationMode::Impl final : IApplicationStateLifecycle {
    explicit Impl(HeartsteadApplicationModeConfig initial_config)
        : config(std::move(initial_config)), states(this),
          widgets(config.content_report == nullptr ? ui::UiSkin::storybook_default()
                                                   : config.content_report->ui_skin) {}

    HeartsteadApplicationModeConfig config;
    ApplicationStateMachine states;
    GameApplicationServices* services = nullptr;
    const GameApplicationFrame* frame = nullptr;
    GameRuntime application_runtime;
    std::optional<GameRuntime> session_runtime;
    std::future<SessionLoadResult> loading;
    std::stop_source loading_stop;
    ui::WidgetTree widgets;
    movement::PlayerCameraRig camera_rig;
    std::optional<RuntimeFrameStats> runtime_stats;
    std::uint64_t frame_count = 0;
    std::uint64_t completed_session_count = 0;
    std::uint64_t next_session_generation = 1;
    std::uint64_t loading_generation = 0;
    bool initialized = false;
    std::optional<core::Error> display_error;

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
            if (!(status = add(label("heartstead.menu.title", "HEARTSTEAD", 36.0F))) ||
                !(status = add(label("heartstead.menu.subtitle",
                                     "A server-authoritative settlement world"))) ||
                !(status = add(button(start_id, "Start Development World",
                                      "Creates a local server and local client")))) {
                return status;
            }
            status = add(button(quit_id, "Quit"));
            if (status) {
                widgets.set_focus(start_id);
            }
            return status;
        case ApplicationState::session_loading:
            if (!(status = add(label("heartstead.loading.title", "Loading world", 28.0F))) ||
                !(status = add(label("heartstead.loading.phase",
                                     "Starting authoritative server and local client...")))) {
                return status;
            }
            status = add(button(cancel_id, "Cancel"));
            if (status) {
                widgets.set_focus(cancel_id);
            }
            return status;
        case ApplicationState::paused:
            if (!(status = add(label("heartstead.pause.title", "Paused", 30.0F))) ||
                !(status = add(button(resume_id, "Resume")))) {
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

    [[nodiscard]] core::Status begin_local_session_loading() {
        if (loading.valid() || session_runtime.has_value()) {
            return core::Status::failure("heartstead.session_already_pending",
                                         "a runtime session is already active or loading");
        }
        loading_stop = std::stop_source{};
        loading_generation = next_session_generation++;
        const auto stop_token = loading_stop.get_token();
        const auto generation = loading_generation;
        const auto* content_report = config.content_report;
        const auto headless = config.headless;
        loading = std::async(
            std::launch::async,
            [content_report, headless, generation, stop_token]() -> SessionLoadResult {
                if (stop_token.stop_requested()) {
                    return SessionLoadResult::failure("heartstead.session_load_cancelled",
                                                      "session loading was cancelled");
                }
                auto runtime = GameRuntime::initialize(GameRuntimeConfig{}, *content_report);
                if (!runtime) {
                    return SessionLoadResult::failure(runtime.error().code,
                                                      runtime.error().message);
                }
                if (stop_token.stop_requested()) {
                    return SessionLoadResult::failure("heartstead.session_load_cancelled",
                                                      "session loading was cancelled");
                }
                auto metadata = content::save_metadata_from_content_report(*content_report, "0.1.0",
                                                                           foundation::world_seed);
                if (!metadata) {
                    return SessionLoadResult::failure(metadata.error().code,
                                                      metadata.error().message);
                }
                SessionLaunchRequest request;
                request.ownership_generation = generation;
                request.mode = SessionMode::local_single_player;
                request.world_source = WorldSourceKind::developer_scenario;
                request.persistence = PersistencePolicy::ephemeral;
                request.world_name = "Development World";
                request.metadata = std::move(metadata).value();
                request.seed = foundation::world_seed;
                request.scenario_id = foundation::scenario_id;
                request.runtime.create_renderer = !headless;
                request.runtime.create_audio = !headless;
                request.runtime.headless = headless;
                request.runtime.physics_backend =
                    headless ? physics::PhysicsBackend::headless : physics::PhysicsBackend::jolt;
                request.runtime.gameplay_modules.push_back(
                    std::make_shared<animals::WanderingAnimalModule>());
                if (stop_token.stop_requested()) {
                    return SessionLoadResult::failure("heartstead.session_load_cancelled",
                                                      "session loading was cancelled");
                }
                auto status = runtime.value().start_session(std::move(request));
                if (!status) {
                    return SessionLoadResult::failure(status.error().code, status.error().message);
                }
                if (stop_token.stop_requested()) {
                    (void)runtime.value().shutdown();
                    return SessionLoadResult::failure("heartstead.session_load_cancelled",
                                                      "session loading was cancelled");
                }
                return SessionLoadResult::success({generation, std::move(runtime).value()});
            });
        return core::Status::ok();
    }

    [[nodiscard]] core::Status take_finished_load(bool enter_game) {
        auto loaded = loading.get();
        if (!loaded) {
            if (!enter_game || loading_stop.stop_requested() ||
                loaded.error().code == "heartstead.session_load_cancelled") {
                return core::Status::ok();
            }
            return states.transition(ApplicationState::load_failure, "session startup failed",
                                     loaded.error());
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
                (void)runtime.shutdown();
                return status;
            }
        }
        session_runtime.emplace(std::move(runtime));
        return states.transition(ApplicationState::in_game, "session startup completed");
    }

    [[nodiscard]] core::Status unload_session() {
        auto first_failure = core::Status::ok();
        if (session_runtime.has_value()) {
            auto status = session_runtime->shutdown();
            if (!status && first_failure) {
                first_failure = status;
            }
            session_runtime.reset();
            runtime_stats.reset();
            ++completed_session_count;
        }
        return first_failure;
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
            return begin_local_session_loading();
        }
        if (state == ApplicationState::session_unloading) {
            loading_stop.request_stop();
            if (session_runtime.has_value() && session_runtime->session() != nullptr) {
                return session_runtime->session()->request_stop();
            }
        }
        return core::Status::ok();
    }

    core::Status update_state(ApplicationState state, std::uint64_t) override {
        switch (state) {
        case ApplicationState::session_loading:
            if (loading.valid() &&
                loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                return take_finished_load(true);
            }
            break;
        case ApplicationState::in_game:
            if (!session_runtime.has_value() || session_runtime->session() == nullptr ||
                frame == nullptr) {
                return core::Status::failure("heartstead.session_missing",
                                             "in-game state requires an active runtime session");
            }
            {
                auto advanced = session_runtime->run_frame(
                    {frame->delta_microseconds, frame->now_milliseconds});
                if (!advanced) {
                    const auto error = advanced.error();
                    auto status = unload_session();
                    if (!status) {
                        return status;
                    }
                    return states.transition(ApplicationState::load_failure,
                                             "runtime session failed", error);
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
                auto status = unload_session();
                if (!status) {
                    return status;
                }
            }
            return states.transition(ApplicationState::main_menu, "session resources released");
        case ApplicationState::shutdown:
            if (services != nullptr) {
                services->request_quit();
            }
            break;
        case ApplicationState::boot:
        case ApplicationState::main_menu:
        case ApplicationState::paused:
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

    [[nodiscard]] core::Status process_input(const GameApplicationFrame& current_frame) {
        if (current_frame.input == nullptr) {
            return core::Status::ok();
        }
        const auto state = states.state();
        if (state == ApplicationState::in_game &&
            key_pressed(*current_frame.input, platform::KeyCode::escape)) {
            return states.transition(ApplicationState::paused, "pause requested");
        }
        if (config.headless || state == ApplicationState::in_game ||
            state == ApplicationState::session_unloading || state == ApplicationState::shutdown) {
            return core::Status::ok();
        }
        auto status = widgets.layout({static_cast<float>(current_frame.extent.width),
                                      static_cast<float>(current_frame.extent.height)});
        if (!status) {
            return status;
        }
        const auto routed =
            widgets.route_input(ui::UiInputFrame::from_platform(*current_frame.input));
        for (const auto& event : routed.events) {
            if (event.kind == ui::UiEventKind::cancelled) {
                if (state == ApplicationState::session_loading) {
                    return states.transition(ApplicationState::session_unloading,
                                             "session loading cancelled");
                }
                if (state == ApplicationState::paused) {
                    return states.transition(ApplicationState::in_game, "pause dismissed");
                }
                if (state == ApplicationState::main_menu) {
                    return states.transition(ApplicationState::shutdown, "menu back requested");
                }
                if (state == ApplicationState::load_failure ||
                    state == ApplicationState::connection_failure) {
                    return states.transition(ApplicationState::main_menu,
                                             "error message dismissed");
                }
            }
            if (event.kind != ui::UiEventKind::clicked) {
                continue;
            }
            if (event.target == start_id) {
                return states.transition(ApplicationState::session_loading,
                                         "development world selected");
            }
            if (event.target == quit_id) {
                return states.transition(ApplicationState::shutdown, "quit selected");
            }
            if (event.target == resume_id) {
                return states.transition(ApplicationState::in_game, "resume selected");
            }
            if (event.target == return_id || event.target == cancel_id) {
                return states.transition(ApplicationState::session_unloading,
                                         event.target == cancel_id ? "session loading cancelled"
                                                                   : "return to menu selected");
            }
            if (event.target == back_id) {
                return states.transition(ApplicationState::main_menu, "error acknowledged");
            }
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Result<renderer::RenderCamera>
    prepare_camera_and_world(const GameApplicationFrame& current_frame) {
        renderer::RenderCamera camera;
        auto status =
            camera.set_aspect_ratio(static_cast<float>(std::max(1U, current_frame.extent.width)) /
                                    static_cast<float>(std::max(1U, current_frame.extent.height)));
        if (!status) {
            return core::Result<renderer::RenderCamera>::failure(status.error().code,
                                                                 status.error().message);
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
            auto camera_frame =
                camera_rig.evaluate(player->state, movement::PlayerCameraPerspective::third_person,
                                    current_frame.extent.width, current_frame.extent.height,
                                    &collision, current_frame.delta_seconds());
            if (!camera_frame) {
                return core::Result<renderer::RenderCamera>::failure(camera_frame.error().code,
                                                                     camera_frame.error().message);
            }
            camera = camera_from_frame(camera_frame.value());
        }
        if (services != nullptr && services->renderer() != nullptr) {
            status = services->renderer()->synchronize_chunks(client->world(), camera);
            if (!status) {
                return core::Result<renderer::RenderCamera>::failure(status.error().code,
                                                                     status.error().message);
            }
        }
        return core::Result<renderer::RenderCamera>::success(camera);
    }

    [[nodiscard]] core::Status paint_ui(const GameApplicationFrame& current_frame) {
        if (config.headless || states.state() == ApplicationState::in_game ||
            states.state() == ApplicationState::shutdown || services == nullptr ||
            services->renderer() == nullptr || services->renderer()->ui_renderer() == nullptr) {
            return core::Status::ok();
        }
        auto status = widgets.layout({static_cast<float>(current_frame.extent.width),
                                      static_cast<float>(current_frame.extent.height)});
        if (!status) {
            return status;
        }
        auto painted = widgets.paint(*services->renderer()->ui_renderer());
        if (!painted) {
            return core::Status::failure(painted.error().code, painted.error().message);
        }
        services->renderer()->set_ui_widget_stats(0.0, 0.0, widgets.layout_stats().widget_count);
        return core::Status::ok();
    }
};

HeartsteadApplicationMode::HeartsteadApplicationMode(HeartsteadApplicationModeConfig config)
    : implementation_(std::make_unique<Impl>(std::move(config))) {}

HeartsteadApplicationMode::~HeartsteadApplicationMode() = default;

core::Status HeartsteadApplicationMode::initialize(GameApplicationServices& services) {
    auto& state = *implementation_;
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
    status = state.states.start();
    if (!status) {
        return status;
    }
    status =
        state.states.transition(ApplicationState::main_menu, "application services initialized");
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
    ++state.frame_count;
    auto status = state.process_input(frame);
    if (status) {
        status = state.states.update(frame.delta_microseconds);
    }
    if (status && services.audio() != nullptr) {
        status = services.audio()->update(frame.delta_seconds());
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
    auto camera = state.prepare_camera_and_world(frame);
    if (!camera) {
        return core::Result<GameApplicationFrameOutput>::failure(camera.error().code,
                                                                 camera.error().message);
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
    auto first_failure = state.unload_session();
    if (state.loading.valid()) {
        state.loading_stop.request_stop();
        state.loading.wait();
        auto status = state.take_finished_load(false);
        if (!status && first_failure) {
            first_failure = status;
        }
    }
    auto status = state.application_runtime.shutdown();
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
           " completed_sessions=" + std::to_string(state.completed_session_count);
}

} // namespace heartstead::game
