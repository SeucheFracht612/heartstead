#include "apps/dev_game/dev_game_mode.hpp"

#include "engine/content/content_validation.hpp"
#include "engine/input/input_action.hpp"
#include "engine/movement/player_camera.hpp"
#include "engine/movement/player_input.hpp"
#include "engine/renderer/environment/day_night.hpp"
#include "engine/save/save_database.hpp"
#include "game/features/animals/wandering_animal_module.hpp"
#include "game/features/interaction/voxel_raycast.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/presentation/client_audio_presentation.hpp"
#include "game/presentation/model_presentation_system.hpp"
#include "game/presentation/particle_presentation.hpp"
#include "game/presentation/voxel_interaction_presentation.hpp"
#include "game/runtime/game_runtime.hpp"
#include "game/ui/game_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace heartstead::dev_game {

namespace {

[[nodiscard]] renderer::RenderCamera render_camera_from(const movement::PlayerCameraFrame& frame) {
    renderer::RenderCamera camera;
    camera.floating_origin = frame.floating_origin;
    camera.local_position = {static_cast<float>(frame.position.local_offset.x),
                             static_cast<float>(frame.position.local_offset.y),
                             static_cast<float>(frame.position.local_offset.z)};
    camera.view = frame.view;
    camera.projection = frame.projection;
    camera.view_projection = frame.view_projection;
    camera.yaw_radians =
        std::atan2(static_cast<float>(frame.forward.x), static_cast<float>(frame.forward.z));
    camera.pitch_radians = std::asin(std::clamp(static_cast<float>(frame.forward.y), -1.0F, 1.0F));
    return camera;
}

[[nodiscard]] core::Status start_runtime(game::GameRuntime& runtime,
                                         const content::ContentValidationReport& content_report,
                                         const DevGameModeConfig& options,
                                         const save::FileSaveDatabase* save_database,
                                         bool& loaded_existing_save) {
    loaded_existing_save = false;
    game::RuntimeConfiguration config;
    config.create_server = !options.connect_endpoint.has_value();
    config.create_client = true;
    config.create_renderer = !options.headless;
    config.create_audio = !options.headless;
    config.use_in_memory_transport = !options.connect_endpoint.has_value();
    config.remote_server_endpoint = options.connect_endpoint;
    config.headless = options.headless;
    config.physics_backend =
        options.headless ? physics::PhysicsBackend::headless : physics::PhysicsBackend::jolt;
    config.gameplay_modules.push_back(std::make_shared<game::animals::WanderingAnimalModule>());
    if (save_database != nullptr) {
        if (options.connect_endpoint.has_value()) {
            return core::Status::failure(
                "dev_game.remote_save_unsupported",
                "a remote client cannot load or write the authoritative world save");
        }
        auto stats = save_database->stats();
        if (!stats) {
            return core::Status::failure(stats.error().code, stats.error().message);
        }
        if (stats.value().has_snapshot) {
            auto status = runtime.start_session_from_save(std::move(config), *save_database);
            loaded_existing_save = status.is_ok();
            return status;
        }
    }

    auto metadata = content::save_metadata_from_content_report(
        content_report, "Foundation Slice 0.1", game::foundation::world_seed);
    if (!metadata) {
        return core::Status::failure(metadata.error().code, metadata.error().message);
    }
    game::SessionRequest request;
    request.metadata = std::move(metadata).value();
    return runtime.start_session(config, std::move(request));
}

} // namespace

struct DevGameMode::Impl {
    explicit Impl(DevGameModeConfig initial_config) : config(std::move(initial_config)) {}

    DevGameModeConfig config;
    game::GameRuntime runtime;
    bool runtime_started = false;
    std::optional<save::FileSaveDatabase> save_database;
    bool loaded_existing_save = false;
    bool wrote_save = false;
    game::ModelPresentationSystem model_presentation;
    bool model_presentation_initialized = false;
    std::optional<renderer::CpuParticleSystem> particle_system;
    renderer::ParticleSystemConfig particle_config;
    game::ParticlePresentation particle_presentation;
    game::VoxelInteractionPresentation voxel_interaction_presentation;
    bool particle_presentation_initialized = false;
    renderer::ParticleEmitterId fire_emitter;
    std::uint64_t particle_seed = 1;
    bool was_swimming = false;
    core::PrototypeId fire_ember;
    core::PrototypeId splash;
    core::PrototypeId clay;
    game::ClientAudioPresentation audio_presentation;
    bool audio_presentation_initialized = false;
    std::unique_ptr<game::GameUiLayer> game_ui;
    input::InputActionMap actions = input::InputActionMap::gameplay_defaults();
    movement::PlayerInputSampler input_sampler;
    movement::PlayerCameraRig camera_rig;
    std::uint64_t input_tick = 0;
    std::uint64_t frame_count = 0;
    std::uint64_t authoritative_tick = 0;
    bool local_client_connected = false;
    movement::PlayerCameraPerspective camera_perspective =
        movement::PlayerCameraPerspective::third_person;
    bool diagnostic_overlay_visible = true;
    bool debug_geometry_visible = false;
};

DevGameMode::DevGameMode(DevGameModeConfig config)
    : implementation_(std::make_unique<Impl>(std::move(config))) {}

DevGameMode::~DevGameMode() = default;

core::Status DevGameMode::initialize(game::GameApplicationServices& services) {
    auto& state = *implementation_;
    if (state.config.content_report == nullptr) {
        return core::Status::failure("dev_game.missing_content",
                                     "development mode requires validated content");
    }
    auto runtime =
        game::GameRuntime::initialize(game::GameRuntimeConfig{}, *state.config.content_report);
    if (!runtime) {
        return core::Status::failure(runtime.error().code, runtime.error().message);
    }
    state.runtime = std::move(runtime).value();
    if (state.config.save_root.has_value()) {
        if (state.config.save_root->empty()) {
            return core::Status::failure("dev_game.invalid_save_root",
                                         "development save root must not be empty");
        }
        state.save_database.emplace(*state.config.save_root);
    }
    auto status = start_runtime(state.runtime, *state.config.content_report, state.config,
                                state.save_database.has_value() ? &*state.save_database : nullptr,
                                state.loaded_existing_save);
    if (!status) {
        return status;
    }
    state.runtime_started = true;
    if (state.config.headless) {
        return core::Status::ok();
    }

    auto* renderer = services.renderer();
    if (renderer == nullptr) {
        return core::Status::failure("dev_game.renderer_missing",
                                     "native development mode requires the application renderer");
    }
    status = state.model_presentation.initialize(
        *renderer, state.config.content_report->visual_definitions, state.config.cooked_asset_root);
    if (!status) {
        return status;
    }
    state.model_presentation_initialized = true;

    state.particle_config.maximum_particles = 8'192;
    state.particle_config.maximum_emitters = 64;
    state.particle_config.maximum_queued_events = 1'024;
    state.particle_config.maximum_spawns_per_update = 2'048;
    auto particle_system = renderer::CpuParticleSystem::create(
        state.particle_config, state.config.content_report->particle_prototypes);
    if (!particle_system) {
        return core::Status::failure(particle_system.error().code, particle_system.error().message);
    }
    state.particle_system.emplace(std::move(particle_system).value());
    status = state.particle_presentation.initialize(
        *renderer, {.maximum_presented_particles = state.particle_config.maximum_particles});
    if (!status) {
        return status;
    }
    state.particle_presentation_initialized = true;

    const auto fire_ember = core::PrototypeId::parse("base:particles/fire_ember");
    const auto splash = core::PrototypeId::parse("base:particles/splash");
    const auto clay = core::PrototypeId::parse("base:voxels/clay");
    if (!fire_ember || !splash || !clay) {
        return core::Status::failure("dev_game.invalid_base_prototype",
                                     "base particle or clay prototype id is invalid");
    }
    state.fire_ember = *fire_ember;
    state.splash = *splash;
    state.clay = *clay;

    auto audio_system = state.runtime.create_audio_system(audio::AudioBackend::miniaudio);
    if (!audio_system) {
        return core::Status::failure(audio_system.error().code, audio_system.error().message);
    }
    status = services.install_audio_system(std::move(audio_system).value());
    if (!status) {
        return status;
    }
    status = state.audio_presentation.initialize(*services.audio());
    if (!status) {
        return status;
    }
    state.audio_presentation_initialized = true;

    state.game_ui = std::make_unique<game::GameUiLayer>(
        state.config.content_report->item_definitions,
        state.config.content_report->entity_definitions, state.config.content_report->ui_skin);
    status = state.game_ui->initialize();
    if (!status) {
        return status;
    }
    auto initial_ui = state.game_ui->synchronize(*state.runtime.session()->client());
    if (!initial_ui) {
        return core::Status::failure(initial_ui.error().code, initial_ui.error().message);
    }
    state.input_sampler.set_orientation(0.0, -2'000.0);
    return services.set_cursor_capture(true);
}

core::Result<game::GameApplicationFrameOutput>
DevGameMode::update(game::GameApplicationServices& services,
                    const game::GameApplicationFrame& frame) {
    auto& state = *implementation_;
    ++state.frame_count;
    if (!state.runtime_started || state.runtime.session() == nullptr) {
        return core::Result<game::GameApplicationFrameOutput>::failure(
            "dev_game.runtime_not_started", "development runtime is not active");
    }

    if (state.config.headless) {
        auto runtime_frame =
            state.runtime.run_frame({frame.delta_microseconds, frame.now_milliseconds});
        if (!runtime_frame) {
            return core::Result<game::GameApplicationFrameOutput>::failure(
                runtime_frame.error().code, runtime_frame.error().message);
        }
        state.authoritative_tick = runtime_frame.value().authoritative_world_tick;
        state.local_client_connected = state.runtime.session()->client()->is_connected();
        return core::Result<game::GameApplicationFrameOutput>::success({});
    }

    auto* renderer = services.renderer();
    auto* audio = services.audio();
    if (renderer == nullptr || audio == nullptr || frame.input == nullptr ||
        state.game_ui == nullptr || !state.particle_system.has_value()) {
        return core::Result<game::GameApplicationFrameOutput>::failure(
            "dev_game.native_services_missing",
            "native development mode is missing renderer, audio, input, UI, or particles");
    }

    auto ui_input = state.game_ui->process_input(*frame.input, *state.runtime.session(),
                                                 frame.now_milliseconds);
    if (!ui_input) {
        return core::Result<game::GameApplicationFrameOutput>::failure(ui_input.error().code,
                                                                       ui_input.error().message);
    }
    if (ui_input.value().inventory_toggled) {
        auto status = services.set_cursor_capture(!state.game_ui->inventory_open());
        if (!status) {
            return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    state.actions.set_context(state.game_ui->inventory_open() ? input::InputContext::inventory
                                                              : input::InputContext::gameplay);
    const auto action_frame = state.actions.evaluate(*frame.input);
    if (!ui_input.value().consumed.keyboard &&
        action_frame[input::InputAction::close_or_pause].pressed) {
        services.request_quit();
    }
    if (!ui_input.value().consumed.keyboard &&
        action_frame[input::InputAction::toggle_camera].pressed) {
        state.camera_perspective =
            state.camera_perspective == movement::PlayerCameraPerspective::third_person
                ? movement::PlayerCameraPerspective::first_person
                : movement::PlayerCameraPerspective::third_person;
    }
    if (!ui_input.value().consumed.keyboard &&
        action_frame[input::InputAction::toggle_debug].pressed) {
        state.diagnostic_overlay_visible = !state.diagnostic_overlay_visible;
    }
    if (!ui_input.value().consumed.keyboard &&
        action_frame[input::InputAction::toggle_debug_geometry].pressed) {
        state.debug_geometry_visible = !state.debug_geometry_visible;
    }

    auto player_input = state.input_sampler.sample(*frame.input, ++state.input_tick);
    const auto* previous_player = state.runtime.session()->client()->local_player_snapshot();
    if (ui_input.value().consumed.blocks_gameplay && previous_player != nullptr) {
        player_input.move_x = 0;
        player_input.move_z = 0;
        player_input.yaw_centidegrees = previous_player->state.yaw_centidegrees;
        player_input.pitch_centidegrees = previous_player->state.pitch_centidegrees;
        player_input.held_buttons = 0;
        player_input.pressed_buttons = 0;
    }
    if (state.runtime.session()->client()->is_connected() && previous_player != nullptr) {
        auto status =
            state.runtime.session()->submit_player_input(player_input, frame.now_milliseconds);
        if (!status) {
            return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                           status.error().message);
        }
    }

    auto runtime_frame =
        state.runtime.run_frame({frame.delta_microseconds, frame.now_milliseconds});
    if (!runtime_frame) {
        return core::Result<game::GameApplicationFrameOutput>::failure(
            runtime_frame.error().code, runtime_frame.error().message);
    }
    state.authoritative_tick = runtime_frame.value().authoritative_world_tick;
    state.local_client_connected = state.runtime.session()->client()->is_connected();

    auto synchronized_ui = state.game_ui->synchronize(*state.runtime.session()->client());
    if (!synchronized_ui) {
        return core::Result<game::GameApplicationFrameOutput>::failure(
            synchronized_ui.error().code, synchronized_ui.error().message);
    }
    if (const auto* server = state.runtime.session()->server(); server != nullptr) {
        renderer->set_voxel_fluid_stats(server->chunk_fluids().stats());
        renderer->set_voxel_lighting_stats(server->chunk_lighting().stats());
    }
    auto day_night = renderer::evaluate_day_night(runtime_frame.value().authoritative_world_tick,
                                                  state.runtime.session()->config().world_time);
    if (!day_night) {
        return core::Result<game::GameApplicationFrameOutput>::failure(day_night.error().code,
                                                                       day_night.error().message);
    }
    auto status = renderer->set_environment(day_night.value().render);
    if (!status) {
        return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                       status.error().message);
    }

    const auto* player = state.runtime.session()->client()->local_player_snapshot();
    if (player == nullptr || !frame.extent.is_valid()) {
        return core::Result<game::GameApplicationFrameOutput>::success({});
    }
    const movement::PlayerCameraCollisionContext camera_collision{
        state.runtime.session()->client()->world().chunks(),
        state.config.content_report->voxel_palette,
    };
    auto camera_frame =
        state.camera_rig.evaluate(player->state, state.camera_perspective, frame.extent.width,
                                  frame.extent.height, &camera_collision);
    if (!camera_frame) {
        return core::Result<game::GameApplicationFrameOutput>::failure(
            camera_frame.error().code, camera_frame.error().message);
    }
    status = state.audio_presentation.update(*audio, player->state, camera_frame.value(),
                                             frame.delta_seconds());
    if (!status) {
        return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                       status.error().message);
    }

    if (!state.fire_emitter.is_valid()) {
        auto fire_position = world::WorldPosition::from_anchor(player->state.position.anchor,
                                                               player->state.position.local_offset +
                                                                   math::Vec3d{2.0, 0.35, 2.0});
        if (!fire_position) {
            return core::Result<game::GameApplicationFrameOutput>::failure(
                fire_position.error().code, fire_position.error().message);
        }
        renderer::ParticleEmitterDesc emitter;
        emitter.prototype_id = state.fire_ember;
        emitter.position = fire_position.value();
        emitter.lifetime_seconds = 3'600.0F;
        emitter.rate_per_second = 18.0F;
        emitter.burst_count = 8;
        emitter.seed = state.particle_seed++;
        auto created_emitter = state.particle_system->create_emitter(emitter);
        if (!created_emitter) {
            return core::Result<game::GameApplicationFrameOutput>::failure(
                created_emitter.error().code, created_emitter.error().message);
        }
        state.fire_emitter = created_emitter.value();
    }

    const auto swimming = player->state.mode == movement::PlayerControllerMode::swimming;
    if (swimming && !state.was_swimming) {
        status = state.particle_system->queue_event({state.splash,
                                                     player->state.position,
                                                     {0.0F, 1.0F, 0.0F},
                                                     {static_cast<float>(player->state.velocity.x),
                                                      static_cast<float>(player->state.velocity.y),
                                                      static_cast<float>(player->state.velocity.z)},
                                                     24,
                                                     state.particle_seed++});
        if (!status) {
            return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                           status.error().message);
        }
    }
    state.was_swimming = swimming;

    status = state.voxel_interaction_presentation.present(
        state.runtime.session()->client()->accepted_voxel_edits(),
        state.config.content_report->voxel_palette, *state.particle_system, *audio);
    if (!status) {
        return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                       status.error().message);
    }

    auto selection = game::interaction::raycast_voxels(
        state.runtime.session()->client()->world().chunks(),
        {camera_frame.value().position, camera_frame.value().forward, 6.0},
        &state.config.content_report->voxel_palette);
    if (!selection) {
        return core::Result<game::GameApplicationFrameOutput>::failure(selection.error().code,
                                                                       selection.error().message);
    }
    if (action_frame[input::InputAction::primary_action].pressed ||
        action_frame[input::InputAction::secondary_action].pressed) {
        const auto& hit = selection.value().hit;
        if (hit.has_value()) {
            if (action_frame[input::InputAction::primary_action].pressed) {
                status = state.runtime.session()->submit_remove_voxel({hit->block},
                                                                      frame.now_milliseconds);
            } else {
                status = state.runtime.session()->submit_place_voxel(
                    {hit->adjacent_block, state.clay}, frame.now_milliseconds);
            }
            if (!status) {
                return core::Result<game::GameApplicationFrameOutput>::failure(
                    status.error().code, status.error().message);
            }
        }
    }

    auto camera = render_camera_from(camera_frame.value());
    if (auto* debug = renderer->debug_renderer(); debug != nullptr) {
        if (selection.value().hit.has_value()) {
            auto block_origin = world::WorldPosition::from_anchor(selection.value().hit->block, {});
            if (!block_origin) {
                return core::Result<game::GameApplicationFrameOutput>::failure(
                    block_origin.error().code, block_origin.error().message);
            }
            const auto* definition = state.config.content_report->voxel_palette.find_by_type(
                selection.value().hit->cell.type);
            if (definition == nullptr) {
                status = debug->submit_aabb(
                    block_origin.value(), {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}},
                    {1.0F, 0.78F, 0.18F, 1.0F}, 0.0F, renderer::DebugDepthMode::overlay);
            } else {
                for (const auto& bounds : definition->selection_bounds) {
                    status =
                        debug->submit_aabb(block_origin.value(), bounds, {1.0F, 0.78F, 0.18F, 1.0F},
                                           0.0F, renderer::DebugDepthMode::overlay);
                    if (!status) {
                        break;
                    }
                }
            }
            if (!status) {
                return core::Result<game::GameApplicationFrameOutput>::failure(
                    status.error().code, status.error().message);
            }
        }
        if (state.debug_geometry_visible) {
            status = debug->submit_aabb(
                player->state.position, {{-0.3F, 0.0F, -0.3F}, {0.3F, 1.8F, 0.3F}},
                player->state.grounded ? std::array{0.20F, 1.0F, 0.36F, 1.0F}
                                       : std::array{1.0F, 0.45F, 0.18F, 1.0F});
            if (status) {
                status = debug->submit_axes(player->state.position, 0.75F);
            }
            const auto speed = math::length(player->state.velocity);
            if (status && speed > 0.01) {
                status = debug->submit_ray(player->state.position,
                                           {static_cast<float>(player->state.velocity.x),
                                            static_cast<float>(player->state.velocity.y),
                                            static_cast<float>(player->state.velocity.z)},
                                           static_cast<float>(speed), {0.25F, 0.72F, 1.0F, 1.0F});
            }
            if (!status) {
                return core::Result<game::GameApplicationFrameOutput>::failure(
                    status.error().code, status.error().message);
            }
        }
    }
    status = state.particle_system->update(frame.delta_seconds());
    if (!status) {
        return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                       status.error().message);
    }
    status = renderer->synchronize_chunks(state.runtime.session()->client()->world(), camera);
    if (!status) {
        return core::Result<game::GameApplicationFrameOutput>::failure(status.error().code,
                                                                       status.error().message);
    }
    auto render_snapshot = state.runtime.capture_render_snapshot();
    if (!render_snapshot) {
        return core::Result<game::GameApplicationFrameOutput>::failure(
            render_snapshot.error().code, render_snapshot.error().message);
    }
    auto model_stats = state.model_presentation.synchronize(*renderer, render_snapshot.value());
    if (!model_stats) {
        return core::Result<game::GameApplicationFrameOutput>::failure(model_stats.error().code,
                                                                       model_stats.error().message);
    }
    auto particle_stats =
        state.particle_presentation.synchronize(*renderer, *state.particle_system, camera);
    if (!particle_stats) {
        return core::Result<game::GameApplicationFrameOutput>::failure(
            particle_stats.error().code, particle_stats.error().message);
    }
    renderer->set_particle_stats(
        state.particle_system->stats(), particle_stats.value().synchronize_ms,
        particle_stats.value().material_groups, particle_stats.value().dropped_particles);
    if (auto* ui = renderer->ui_renderer(); ui != nullptr) {
        auto painted_ui = state.game_ui->paint(*ui, frame.extent);
        if (!painted_ui) {
            return core::Result<game::GameApplicationFrameOutput>::failure(
                painted_ui.error().code, painted_ui.error().message);
        }
        renderer->set_ui_widget_stats(state.game_ui->stats().layout_ms,
                                      state.game_ui->stats().paint_ms,
                                      state.game_ui->stats().layout.widget_count);
        if (state.diagnostic_overlay_visible) {
            const auto position = player->state.position.approximate_global();
            std::ostringstream text;
            text << std::fixed << std::setprecision(2) << "FOUNDATION DIAGNOSTICS [F3]\n"
                 << "session "
                 << (state.runtime.session()->server() == nullptr ? "remote" : "local")
                 << " | client " << (state.local_client_connected ? "connected" : "offline")
                 << " | tick " << state.authoritative_tick << '\n'
                 << "camera "
                 << (state.camera_perspective == movement::PlayerCameraPerspective::third_person
                         ? "third-person"
                         : "first-person")
                 << " [F1] | geometry " << (state.debug_geometry_visible ? "on" : "off")
                 << " [F4]\n"
                 << "position " << position.x << ", " << position.y << ", " << position.z << '\n'
                 << "velocity " << player->state.velocity.x << ", " << player->state.velocity.y
                 << ", " << player->state.velocity.z << '\n'
                 << "controller " << movement::player_controller_mode_name(player->state.mode)
                 << " | grounded " << (player->state.grounded ? "yes" : "no") << '\n'
                 << "input " << player_input.move_x << ", " << player_input.move_z << " | selected "
                 << (selection.value().hit.has_value() ? "voxel" : "none");
            const auto diagnostic_text = text.str();
            renderer::UiQuadDesc panel;
            panel.minimum_pixels = {12.0F, 12.0F};
            panel.maximum_pixels = {
                std::min(510.0F, static_cast<float>(frame.extent.width) - 12.0F), 112.0F};
            panel.color = {0.015F, 0.025F, 0.04F, 0.88F};
            status = ui->submit_quad(panel);
            if (status) {
                status = ui->submit_text(
                    {{20.0F, 20.0F}, diagnostic_text, 11.0F, {0.82F, 0.92F, 1.0F, 1.0F}});
            }
            if (!status) {
                return core::Result<game::GameApplicationFrameOutput>::failure(
                    status.error().code, status.error().message);
            }
        }
    }

    game::GameApplicationFrameOutput output;
    output.render = renderer::RenderFrameInput{
        camera, static_cast<float>(runtime_frame.value().fixed_step.interpolation_alpha),
        frame.delta_seconds()};
    return core::Result<game::GameApplicationFrameOutput>::success(std::move(output));
}

core::Status DevGameMode::shutdown(game::GameApplicationServices& services) {
    auto& state = *implementation_;
    core::Status first_failure = core::Status::ok();
    const auto remember_failure = [&first_failure](core::Status status) {
        if (!status && first_failure) {
            first_failure = std::move(status);
        }
    };

    if (state.audio_presentation_initialized && services.audio() != nullptr) {
        remember_failure(state.audio_presentation.shutdown(*services.audio()));
        state.audio_presentation_initialized = false;
    }
    if (auto* renderer = services.renderer(); renderer != nullptr) {
        if (state.particle_presentation_initialized) {
            remember_failure(state.particle_presentation.shutdown(*renderer));
            state.particle_presentation_initialized = false;
        }
        if (state.model_presentation_initialized) {
            remember_failure(state.model_presentation.shutdown(*renderer));
            state.model_presentation_initialized = false;
        }
    }
    state.game_ui.reset();
    state.particle_system.reset();
    if (state.runtime_started) {
        if (state.save_database.has_value() && state.runtime.session() != nullptr &&
            state.runtime.session()->server() != nullptr) {
            auto status = state.runtime.save_to(*state.save_database);
            if (status) {
                state.wrote_save = true;
            }
            remember_failure(std::move(status));
        }
        remember_failure(state.runtime.shutdown());
        state.runtime_started = false;
    }
    return first_failure;
}

std::string DevGameMode::summary() const {
    const auto& state = *implementation_;
    return "development runtime: frames=" + std::to_string(state.frame_count) +
           " authoritative_tick=" + std::to_string(state.authoritative_tick) +
           " local_client=" + (state.local_client_connected ? "connected" : "offline") + " save=" +
           (state.wrote_save                  ? "written"
            : state.loaded_existing_save      ? "loaded"
            : state.save_database.has_value() ? "new"
                                              : "disabled");
}

} // namespace heartstead::dev_game
