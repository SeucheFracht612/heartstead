#include "engine/assets/asset_cooker.hpp"
#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/model_asset.hpp"
#include "engine/audio/audio_system.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/logging.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/entities/physical_resource.hpp"
#include "engine/input/input_action.hpp"
#include "engine/movement/player_camera.hpp"
#include "engine/movement/player_input.hpp"
#include "engine/net/transport.hpp"
#include "engine/platform/platform.hpp"
#include "engine/renderer/environment/day_night.hpp"
#include "engine/renderer/renderer.hpp"
#include "engine/renderer/shaders/spirv_loader.hpp"
#include "game/features/animals/wandering_animal_module.hpp"
#include "game/features/interaction/voxel_raycast.hpp"
#include "game/presentation/animated_model_presentation.hpp"
#include "game/presentation/client_audio_presentation.hpp"
#include "game/presentation/particle_presentation.hpp"
#include "game/runtime/game_runtime.hpp"
#include "game/ui/game_ui.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace heartstead;

struct LaunchOptions {
    bool headless = false;
    std::optional<std::uint32_t> maximum_frames;
    std::optional<net::TransportEndpoint> connect_endpoint;
    bool help = false;
};

core::Result<LaunchOptions> parse_options(int argc, char** argv) {
    LaunchOptions options;
    for (int index = 1; index < argc; ++index) {
        const auto argument = std::string_view(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--frames") {
            if (index + 1 >= argc) {
                return core::Result<LaunchOptions>::failure("dev_game.missing_frame_count",
                                                            "--frames requires a positive integer");
            }
            const auto value = std::string_view(argv[++index]);
            std::uint32_t frames = 0;
            const auto [end, error] =
                std::from_chars(value.data(), value.data() + value.size(), frames);
            if (error != std::errc{} || end != value.data() + value.size() || frames == 0) {
                return core::Result<LaunchOptions>::failure(
                    "dev_game.invalid_frame_count", "--frames requires a positive 32-bit integer");
            }
            options.maximum_frames = frames;
            // CI smoke runs historically use only --frames and must not require a display.
            options.headless = true;
        } else if (argument == "--connect") {
            if (index + 1 >= argc) {
                return core::Result<LaunchOptions>::failure(
                    "dev_game.missing_connect_endpoint",
                    "--connect requires ADDRESS:PORT");
            }
            auto endpoint = net::parse_transport_endpoint(argv[++index], 7777);
            if (!endpoint) {
                return core::Result<LaunchOptions>::failure(endpoint.error().code,
                                                            endpoint.error().message);
            }
            options.connect_endpoint = std::move(endpoint).value();
        } else {
            return core::Result<LaunchOptions>::failure("dev_game.unknown_option",
                                                        "unknown option: " + std::string(argument));
        }
    }
    return core::Result<LaunchOptions>::success(std::move(options));
}

void print_usage(const char* executable, std::ostream& output) {
    output << "usage: " << executable
           << " [--headless] [--frames N] [--connect ADDRESS:PORT]\n"
           << "       --frames implies --headless for deterministic smoke runs\n";
}

int fail(const core::Error& error) {
    std::cerr << error.code << ": " << error.message << '\n';
    return 1;
}

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

core::Result<game::GameRuntime>
create_runtime(const content::ContentValidationReport& content_report) {
    return game::GameRuntime::initialize(game::GameRuntimeConfig{}, content_report);
}

core::Status start_runtime(game::GameRuntime& runtime,
                           const content::ContentValidationReport& content_report,
                           const LaunchOptions& options) {
    auto metadata = content::save_metadata_from_content_report(content_report, "development",
                                                               0x4845415254535445ULL);
    if (!metadata) {
        return core::Status::failure(metadata.error().code, metadata.error().message);
    }
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
    game::SessionRequest request;
    request.metadata = std::move(metadata).value();
    return runtime.start_session(config, std::move(request));
}

int run_headless(game::GameRuntime& runtime, std::uint32_t frame_count) {
    std::uint64_t simulated_us = 0;
    game::RuntimeFrameStats last_frame;
    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        simulated_us += 16'667;
        auto result = runtime.run_frame({16'667, static_cast<std::int64_t>(simulated_us / 1000U)});
        if (!result) {
            return fail(result.error());
        }
        last_frame = std::move(result).value();
    }
    std::cout << "development runtime: frames=" << frame_count
              << " authoritative_tick=" << last_frame.authoritative_world_tick << " local_client="
              << (runtime.session()->client()->is_connected() ? "connected" : "offline") << '\n';
    auto status = runtime.shutdown();
    return status ? 0 : fail(status.error());
}

struct ShaderSet {
    std::vector<std::uint32_t> sky_vertex;
    std::vector<std::uint32_t> sky_fragment;
    std::vector<std::uint32_t> terrain_vertex;
    std::vector<std::uint32_t> terrain_fragment;
    std::vector<std::uint32_t> static_vertex;
    std::vector<std::uint32_t> static_fragment;
    std::vector<std::uint32_t> debug_vertex;
    std::vector<std::uint32_t> debug_fragment;
    std::vector<std::uint32_t> ui_vertex;
    std::vector<std::uint32_t> ui_fragment;
};

struct DevPhysicalResourceVisual {
    core::SaveId resource_id;
    renderer::RenderObjectId render_id;
    math::Transform3f transform;
};

core::Result<renderer::RenderObjectProxy>
physical_resource_proxy(const entities::PhysicalResourceRecord& resource,
                        renderer::Renderer& renderer,
                        const DevPhysicalResourceVisual* previous = nullptr) {
    const auto origin = world::WorldPosition::from_anchor({}, {});
    if (!origin) {
        return core::Result<renderer::RenderObjectProxy>::failure(origin.error().code,
                                                                  origin.error().message);
    }
    const auto global = resource.position.approximate_global();
    renderer::RenderObjectProxy proxy;
    proxy.id = previous == nullptr ? renderer::RenderObjectId{} : previous->render_id;
    proxy.anchor = origin.value();
    proxy.current_transform.position = {static_cast<float>(global.x), static_cast<float>(global.y),
                                        static_cast<float>(global.z)};
    proxy.current_transform.rotation_degrees = resource.rotation_degrees;
    proxy.current_transform.scale = {1.2F, 0.4F, 0.4F};
    proxy.previous_transform = previous == nullptr ? proxy.current_transform : previous->transform;
    proxy.mesh = renderer.fallback_mesh();
    proxy.material = {1, 1};
    proxy.local_bounds = {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
    proxy.flags = renderer::RenderObjectFlags::cast_shadow;
    proxy.color = {0.42F, 0.20F, 0.07F, 1.0F};
    return core::Result<renderer::RenderObjectProxy>::success(proxy);
}

core::Status drop_demo_resource(game::GameRuntime& runtime, renderer::Renderer& renderer,
                                const movement::PlayerCameraFrame& camera,
                                std::vector<DevPhysicalResourceVisual>& visuals) {
    auto* session = runtime.session();
    auto* server = session == nullptr ? nullptr : session->server();
    if (server == nullptr) {
        return core::Status::failure("dev_game.server_missing",
                                     "dropping a resource requires the local authority");
    }
    auto resource_id = server->world().save_ids().reserve();
    if (!resource_id) {
        return core::Status::failure(resource_id.error().code, resource_id.error().message);
    }
    auto position = world::WorldPosition::from_anchor(
        camera.position.anchor,
        camera.position.local_offset + camera.forward * 1.5 + math::Vec3d{0.0, -0.25, 0.0});
    if (!position) {
        return core::Status::failure(position.error().code, position.error().message);
    }
    entities::PhysicalResourceRecord resource;
    resource.resource_id = resource_id.value();
    resource.prototype_id = *core::PrototypeId::parse("base:entities/dropped_log");
    resource.cargo_prototype_id = *core::PrototypeId::parse("base:cargo/heavy_log");
    resource.position = position.value();
    resource.kind = entities::PhysicalResourceKind::haulable_log;
    resource.mass_grams = 12'000;
    resource.volume_milliliters = 24'000;
    resource.allowed_transport_modes = cargo::CargoTransportModes::of(
        {cargo::CargoTransportMode::hand, cargo::CargoTransportMode::cart});
    resource.segments.push_back({physics::ShapeKind::box, {}, {0.6F, 0.2F, 0.2F}, 0.5F, 0.5F});
    const physics::Vec3 velocity{static_cast<float>(camera.forward.x * 4.0),
                                 static_cast<float>(camera.forward.y * 4.0 + 1.5),
                                 static_cast<float>(camera.forward.z * 4.0)};
    auto status = server->drop_physical_resource(resource, velocity, {0.0F, 0.0F, 2.0F});
    if (!status) {
        return status;
    }
    const auto* dropped = server->world().physical_resources().find(resource_id.value());
    if (dropped == nullptr) {
        return core::Status::failure("dev_game.resource_missing",
                                     "dropped resource was not retained by the authority");
    }
    auto proxy = physical_resource_proxy(*dropped, renderer);
    if (!proxy) {
        return core::Status::failure(proxy.error().code, proxy.error().message);
    }
    auto created = renderer.create_object(proxy.value());
    if (!created) {
        return core::Status::failure(created.error().code, created.error().message);
    }
    visuals.push_back({resource_id.value(), created.value(), proxy.value().current_transform});
    return core::Status::ok();
}

core::Status synchronize_demo_resources(const game::GameRuntime& runtime,
                                        renderer::Renderer& renderer,
                                        std::vector<DevPhysicalResourceVisual>& visuals) {
    const auto* session = runtime.session();
    const auto* server = session == nullptr ? nullptr : session->server();
    if (server == nullptr) {
        return core::Status::ok();
    }
    std::vector<renderer::RenderSceneUpdate> updates;
    updates.reserve(visuals.size());
    for (auto& visual : visuals) {
        const auto* resource = server->world().physical_resources().find(visual.resource_id);
        if (resource == nullptr) {
            continue;
        }
        auto proxy = physical_resource_proxy(*resource, renderer, &visual);
        if (!proxy) {
            return core::Status::failure(proxy.error().code, proxy.error().message);
        }
        renderer::RenderSceneUpdate update;
        update.kind = renderer::RenderSceneUpdateKind::upsert_object;
        update.object = proxy.value();
        updates.push_back(std::move(update));
        visual.transform = proxy.value().current_transform;
    }
    return renderer.apply_scene_updates(updates);
}

core::Result<ShaderSet> load_shaders() {
    const auto root = std::filesystem::path{HEARTSTEAD_DEV_GAME_ASSET_DIR} / "shaders";
    const std::array paths{
        root / "sky.vert.spv",        root / "sky.frag.spv",         root / "terrain.vert.spv",
        root / "terrain.frag.spv",    root / "static_mesh.vert.spv", root / "static_mesh.frag.spv",
        root / "debug_line.vert.spv", root / "debug_line.frag.spv",  root / "ui.vert.spv",
        root / "ui.frag.spv",
    };
    std::array<core::Result<std::vector<std::uint32_t>>, 10> loaded{
        renderer::shaders::load_spirv_file(paths[0]), renderer::shaders::load_spirv_file(paths[1]),
        renderer::shaders::load_spirv_file(paths[2]), renderer::shaders::load_spirv_file(paths[3]),
        renderer::shaders::load_spirv_file(paths[4]), renderer::shaders::load_spirv_file(paths[5]),
        renderer::shaders::load_spirv_file(paths[6]), renderer::shaders::load_spirv_file(paths[7]),
        renderer::shaders::load_spirv_file(paths[8]), renderer::shaders::load_spirv_file(paths[9]),
    };
    for (std::size_t index = 0; index < loaded.size(); ++index) {
        if (!loaded[index]) {
            return core::Result<ShaderSet>::failure(loaded[index].error().code,
                                                    "failed to load " + paths[index].string() +
                                                        ": " + loaded[index].error().message);
        }
    }
    ShaderSet result;
    result.sky_vertex = std::move(loaded[0]).value();
    result.sky_fragment = std::move(loaded[1]).value();
    result.terrain_vertex = std::move(loaded[2]).value();
    result.terrain_fragment = std::move(loaded[3]).value();
    result.static_vertex = std::move(loaded[4]).value();
    result.static_fragment = std::move(loaded[5]).value();
    result.debug_vertex = std::move(loaded[6]).value();
    result.debug_fragment = std::move(loaded[7]).value();
    result.ui_vertex = std::move(loaded[8]).value();
    result.ui_fragment = std::move(loaded[9]).value();
    return core::Result<ShaderSet>::success(std::move(result));
}

core::Result<assets::ModelAsset> load_storybook_player_model() {
    constexpr std::string_view logical_id = "base:models/entities/storybook_player.gltf";
    auto store =
        assets::CookedAssetStore::load(std::filesystem::path{HEARTSTEAD_DEV_GAME_COOKED_ASSET_DIR});
    if (!store) {
        return core::Result<assets::ModelAsset>::failure(store.error().code, store.error().message);
    }
    auto payload = store.value().load_payload(logical_id);
    if (!payload) {
        return core::Result<assets::ModelAsset>::failure(payload.error().code,
                                                         payload.error().message);
    }
    const auto production_model_pipeline = assets::asset_cook_pipeline_name(
        assets::AssetKind::model, assets::AssetCookBackend::production_converters);
    if (payload.value().kind != assets::AssetKind::model ||
        payload.value().profile != "production" ||
        payload.value().backend != production_model_pipeline) {
        return core::Result<assets::ModelAsset>::failure(
            "dev_game.invalid_character_asset",
            "storybook player must be a production-cooked model asset");
    }
    return assets::decode_model_asset(payload.value().bytes);
}

renderer::RenderCamera render_camera_from(const movement::PlayerCameraFrame& frame) {
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
    camera.pitch_radians = std::asin(
        std::clamp(static_cast<float>(frame.forward.y), -1.0F, 1.0F));
    return camera;
}

int run_native(game::GameRuntime& runtime, const content::ContentValidationReport& content_report,
               const LaunchOptions& options) {
    using namespace renderer::rhi;
    auto active_platform = platform::create_platform({platform::PlatformBackend::native});
    if (!active_platform) {
        return fail(active_platform.error());
    }
    RenderExtent extent{1280, 720};
    auto window = active_platform.value()->create_window(
        {"Heartstead Development Game", extent.width, extent.height, true});
    if (!window) {
        return fail(window.error());
    }
    auto native_handle = active_platform.value()->native_window_handle(window.value());
    if (!native_handle) {
        return fail("native platform did not expose a Vulkan window handle");
    }
    RenderDeviceDesc device_desc;
    device_desc.backend = RenderBackend::vulkan;
    device_desc.application_name = "Heartstead Development Game";
    device_desc.initial_extent = extent;
    device_desc.present_mode = PresentMode::fifo;
    device_desc.enable_validation = true;
    device_desc.native_window = *native_handle;
    auto device = create_render_device(device_desc);
    if (!device) {
        return fail(device.error());
    }
    auto shaders = load_shaders();
    if (!shaders) {
        return fail(shaders.error());
    }
    renderer::RendererInitDesc renderer_desc;
    renderer_desc.device = std::move(device).value();
    renderer_desc.sky_vertex_spirv = std::move(shaders.value().sky_vertex);
    renderer_desc.sky_fragment_spirv = std::move(shaders.value().sky_fragment);
    renderer_desc.terrain_vertex_spirv = std::move(shaders.value().terrain_vertex);
    renderer_desc.terrain_fragment_spirv = std::move(shaders.value().terrain_fragment);
    renderer_desc.static_mesh_vertex_spirv = std::move(shaders.value().static_vertex);
    renderer_desc.static_mesh_fragment_spirv = std::move(shaders.value().static_fragment);
    renderer_desc.debug_vertex_spirv = std::move(shaders.value().debug_vertex);
    renderer_desc.debug_fragment_spirv = std::move(shaders.value().debug_fragment);
    renderer_desc.ui_vertex_spirv = std::move(shaders.value().ui_vertex);
    renderer_desc.ui_fragment_spirv = std::move(shaders.value().ui_fragment);
    renderer_desc.voxel_palette = &content_report.voxel_palette;
    renderer::Renderer renderer;
    auto status = renderer.initialize(std::move(renderer_desc));
    if (!status) {
        return fail(status.error());
    }
    auto character_model = load_storybook_player_model();
    if (!character_model) {
        return fail(character_model.error());
    }
    game::AnimatedModelPresentationConfig animated_model_config;
    animated_model_config.asset_id = "base:models/entities/storybook_player.gltf";
    animated_model_config.visual_prototype = *core::PrototypeId::parse("base:entities/player");
    animated_model_config.model = std::move(character_model).value();
    animated_model_config.locomotion_clips = {0, 1, 2, 9};
    animated_model_config.animated_bounds = animated_model_config.model.bounds.expanded(0.4F);
    animated_model_config.flags =
        renderer::RenderObjectFlags::cast_shadow | renderer::RenderObjectFlags::two_sided;
    animated_model_config.color = {0.76F, 0.39F, 0.20F, 1.0F};
    game::AnimatedModelPresentation animated_models;
    status = animated_models.initialize(renderer, std::move(animated_model_config));
    if (!status) {
        return fail(status.error());
    }
    auto animal_model = load_storybook_player_model();
    if (!animal_model) {
        return fail(animal_model.error());
    }
    game::AnimatedModelPresentationConfig animal_model_config;
    animal_model_config.asset_id = "base:models/entities/storybook_player.gltf#test_animal";
    animal_model_config.visual_prototype = *core::PrototypeId::parse("base:entities/test_animal");
    animal_model_config.model = std::move(animal_model).value();
    animal_model_config.locomotion_clips = {0, 1, 2, 9};
    animal_model_config.animated_bounds = animal_model_config.model.bounds.expanded(0.4F);
    animal_model_config.flags =
        renderer::RenderObjectFlags::cast_shadow | renderer::RenderObjectFlags::two_sided;
    animal_model_config.color = {0.83F, 0.70F, 0.42F, 1.0F};
    game::AnimatedModelPresentation animated_animals;
    status = animated_animals.initialize(renderer, std::move(animal_model_config));
    if (!status) {
        return fail(status.error());
    }
    renderer::ParticleSystemConfig particle_config;
    particle_config.maximum_particles = 8'192;
    particle_config.maximum_emitters = 64;
    particle_config.maximum_queued_events = 1'024;
    particle_config.maximum_spawns_per_update = 2'048;
    auto particle_system =
        renderer::CpuParticleSystem::create(particle_config, content_report.particle_prototypes);
    if (!particle_system) {
        return fail(particle_system.error());
    }
    game::ParticlePresentation particle_presentation;
    status = particle_presentation.initialize(
        renderer, {.maximum_presented_particles = particle_config.maximum_particles});
    if (!status) {
        return fail(status.error());
    }
    const auto fire_ember = core::PrototypeId::parse("base:particles/fire_ember");
    const auto block_break_puff = core::PrototypeId::parse("base:particles/block_break_puff");
    const auto splash = core::PrototypeId::parse("base:particles/splash");
    if (!fire_ember || !block_break_puff || !splash) {
        return fail("base particle prototype ids are invalid");
    }
    renderer::ParticleEmitterId fire_emitter;
    std::uint64_t particle_seed = 1;
    bool was_swimming = false;
    auto audio_system = runtime.create_audio_system(audio::AudioBackend::miniaudio);
    if (!audio_system) {
        return fail(audio_system.error());
    }
    game::ClientAudioPresentation audio_presentation;
    status = audio_presentation.initialize(*audio_system.value());
    if (!status) {
        return fail(status.error());
    }
    game::GameUiLayer game_ui(content_report.item_definitions,
                              content_report.entity_definitions, content_report.ui_skin);
    status = game_ui.initialize();
    if (!status) {
        return fail(status.error());
    }
    auto initial_ui = game_ui.synchronize(*runtime.session()->client());
    if (!initial_ui) {
        return fail(initial_ui.error());
    }

    status = active_platform.value()->set_cursor_capture(window.value(), true);
    if (!status) {
        return fail(status.error());
    }
    auto actions = input::InputActionMap::gameplay_defaults();
    movement::PlayerInputSampler input_sampler;
    input_sampler.set_orientation(0.0, -2'000.0);
    movement::PlayerCameraRig camera_rig;
    std::vector<DevPhysicalResourceVisual> physical_resource_visuals;
    const auto clay = core::PrototypeId::parse("base:voxels/clay");
    if (!clay.has_value()) {
        return fail("base clay prototype is unavailable");
    }

    auto previous_time = std::chrono::steady_clock::now();
    std::uint64_t frame_count = 0;
    std::uint64_t input_tick = 0;
    while (!active_platform.value()->should_quit() &&
           (!options.maximum_frames.has_value() || frame_count < *options.maximum_frames)) {
        active_platform.value()->begin_frame();
        while (auto event = active_platform.value()->poll_event()) {
            if (event->kind == platform::PlatformEventKind::quit_requested ||
                event->kind == platform::PlatformEventKind::window_closed) {
                active_platform.value()->request_quit();
            } else if (event->kind == platform::PlatformEventKind::window_resized &&
                       event->window_id == window.value()) {
                extent = {event->width, event->height};
                if (extent.is_valid()) {
                    status = renderer.resize(extent);
                    if (!status) {
                        return fail(status.error());
                    }
                }
            }
        }
        if (active_platform.value()->should_quit()) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - previous_time);
        previous_time = now;
        const auto frame_us =
            static_cast<std::uint64_t>(std::clamp<std::int64_t>(elapsed.count(), 1, 100'000));
        auto input_snapshot = active_platform.value()->input_snapshot(window.value());
        if (!input_snapshot) {
            return fail("platform did not provide an input snapshot");
        }
        auto ui_input = game_ui.process_input(*input_snapshot, *runtime.session(),
                                              active_platform.value()->clock().now_ms());
        if (!ui_input) {
            return fail(ui_input.error());
        }
        if (ui_input.value().inventory_toggled) {
            status = active_platform.value()->set_cursor_capture(window.value(),
                                                                 !game_ui.inventory_open());
            if (!status) {
                return fail(status.error());
            }
        }
        actions.set_context(game_ui.inventory_open() ? input::InputContext::inventory
                                                     : input::InputContext::gameplay);
        const auto action_frame = actions.evaluate(*input_snapshot);
        if (!ui_input.value().consumed.keyboard &&
            action_frame[input::InputAction::close_or_pause].pressed) {
            active_platform.value()->request_quit();
        }
        auto player_input = input_sampler.sample(*input_snapshot, ++input_tick);
        const auto* previous_player =
            runtime.session()->client()->local_player_snapshot();
        if (ui_input.value().consumed.blocks_gameplay && previous_player != nullptr) {
            player_input.move_x = 0;
            player_input.move_z = 0;
            player_input.yaw_centidegrees = previous_player->state.yaw_centidegrees;
            player_input.pitch_centidegrees = previous_player->state.pitch_centidegrees;
            player_input.held_buttons = 0;
            player_input.pressed_buttons = 0;
        }
        if (runtime.session()->client()->is_connected() &&
            previous_player != nullptr) {
            status = runtime.session()->submit_player_input(
                player_input, active_platform.value()->clock().now_ms());
            if (!status) {
                return fail(status.error());
            }
        }
        auto runtime_frame =
            runtime.run_frame({frame_us, active_platform.value()->clock().now_ms()});
        if (!runtime_frame) {
            return fail(runtime_frame.error());
        }
        auto synchronized_ui = game_ui.synchronize(*runtime.session()->client());
        if (!synchronized_ui) {
            return fail(synchronized_ui.error());
        }
        if (const auto* server = runtime.session()->server(); server != nullptr) {
            renderer.set_voxel_fluid_stats(server->chunk_fluids().stats());
            renderer.set_voxel_lighting_stats(server->chunk_lighting().stats());
        }
        auto day_night = renderer::evaluate_day_night(
            runtime_frame.value().authoritative_world_tick, runtime.session()->config().world_time);
        if (!day_night) {
            return fail(day_night.error());
        }
        status = renderer.set_environment(day_night.value().render);
        if (!status) {
            return fail(status.error());
        }
        const auto* player = runtime.session()->client()->local_player_snapshot();
        if (player == nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            ++frame_count;
            continue;
        }
        if (extent.is_valid()) {
            auto camera_frame =
                camera_rig.evaluate(player->state, movement::PlayerCameraPerspective::third_person,
                                    extent.width, extent.height);
            if (!camera_frame) {
                return fail(camera_frame.error());
            }
            status = audio_presentation.update(*audio_system.value(), player->state,
                                               camera_frame.value(),
                                               static_cast<float>(frame_us) / 1'000'000.0F);
            if (!status) {
                return fail(status.error());
            }
            if (!fire_emitter.is_valid()) {
                auto fire_position = world::WorldPosition::from_anchor(
                    player->state.position.anchor,
                    player->state.position.local_offset + math::Vec3d{2.0, 0.35, 2.0});
                if (!fire_position) {
                    return fail(fire_position.error());
                }
                renderer::ParticleEmitterDesc emitter;
                emitter.prototype_id = *fire_ember;
                emitter.position = fire_position.value();
                emitter.lifetime_seconds = 3'600.0F;
                emitter.rate_per_second = 18.0F;
                emitter.burst_count = 8;
                emitter.seed = particle_seed++;
                auto created_emitter = particle_system.value().create_emitter(emitter);
                if (!created_emitter) {
                    return fail(created_emitter.error());
                }
                fire_emitter = created_emitter.value();
            }
            const auto swimming =
                player->state.mode == movement::PlayerControllerMode::swimming;
            if (swimming && !was_swimming) {
                status = particle_system.value().queue_event(
                    {*splash, player->state.position, {0.0F, 1.0F, 0.0F},
                     {static_cast<float>(player->state.velocity.x),
                      static_cast<float>(player->state.velocity.y),
                      static_cast<float>(player->state.velocity.z)},
                     24, particle_seed++});
                if (!status) {
                    return fail(status.error());
                }
            }
            was_swimming = swimming;
            if (action_frame[input::InputAction::primary_action].pressed ||
                action_frame[input::InputAction::secondary_action].pressed) {
                auto selection = game::interaction::raycast_voxels(
                    runtime.session()->client()->world().chunks(),
                    {camera_frame.value().position, camera_frame.value().forward, 6.0});
                if (!selection) {
                    return fail(selection.error());
                }
                const auto& hit = selection.value().hit;
                if (hit.has_value()) {
                    if (action_frame[input::InputAction::primary_action].pressed) {
                        status = runtime.session()->submit_remove_voxel(
                            {hit->block}, active_platform.value()->clock().now_ms());
                    } else {
                        status = runtime.session()->submit_place_voxel(
                            {hit->adjacent_block, *clay},
                            active_platform.value()->clock().now_ms());
                    }
                    if (!status) {
                        return fail(status.error());
                    }
                    if (action_frame[input::InputAction::primary_action].pressed) {
                        auto puff_position = world::WorldPosition::from_anchor(
                            hit->block, {0.5, 0.5, 0.5});
                        if (!puff_position) {
                            return fail(puff_position.error());
                        }
                        status = particle_system.value().queue_event(
                            {*block_break_puff, puff_position.value(), {0.0F, 1.0F, 0.0F}, {},
                             18, particle_seed++});
                        if (!status) {
                            return fail(status.error());
                        }
                    }
                }
            }
            if (action_frame[input::InputAction::drop_item].pressed) {
                if (runtime.session()->server() != nullptr) {
                    status = drop_demo_resource(runtime, renderer, camera_frame.value(),
                                                physical_resource_visuals);
                    if (!status) {
                        return fail(status.error());
                    }
                }
            }
            auto camera = render_camera_from(camera_frame.value());
            status = particle_system.value().update(
                static_cast<float>(frame_us) / 1'000'000.0F);
            if (!status) {
                return fail(status.error());
            }
            status = renderer.synchronize_chunks(runtime.session()->client()->world(), camera);
            if (!status) {
                return fail(status.error());
            }
            status = synchronize_demo_resources(runtime, renderer, physical_resource_visuals);
            if (!status) {
                return fail(status.error());
            }
            auto render_snapshot = runtime.capture_render_snapshot();
            if (!render_snapshot) {
                return fail(render_snapshot.error());
            }
            auto animated_stats = animated_models.synchronize(renderer, render_snapshot.value());
            if (!animated_stats) {
                return fail(animated_stats.error());
            }
            animated_stats = animated_animals.synchronize(renderer, render_snapshot.value());
            if (!animated_stats) {
                return fail(animated_stats.error());
            }
            auto particle_stats =
                particle_presentation.synchronize(renderer, particle_system.value(), camera);
            if (!particle_stats) {
                return fail(particle_stats.error());
            }
            renderer.set_particle_stats(
                particle_system.value().stats(), particle_stats.value().synchronize_ms,
                particle_stats.value().material_groups, particle_stats.value().dropped_particles);
            if (auto* ui = renderer.ui_renderer(); ui != nullptr) {
                auto painted_ui = game_ui.paint(*ui, extent);
                if (!painted_ui) {
                    return fail(painted_ui.error());
                }
                renderer.set_ui_widget_stats(
                    game_ui.stats().layout_ms, game_ui.stats().paint_ms,
                    game_ui.stats().layout.widget_count);
            }
            auto rendered = renderer.render_frame(
                {camera, static_cast<float>(runtime_frame.value().fixed_step.interpolation_alpha),
                 static_cast<float>(frame_us) / 1'000'000.0F});
            if (!rendered) {
                return fail(rendered.error());
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        ++frame_count;
    }

    status = audio_presentation.shutdown(*audio_system.value());
    if (!status) {
        return fail(status.error());
    }
    audio_system.value().reset();
    status = particle_presentation.shutdown(renderer);
    if (!status) {
        return fail(status.error());
    }
    status = animated_models.shutdown(renderer);
    if (!status) {
        return fail(status.error());
    }
    status = animated_animals.shutdown(renderer);
    if (!status) {
        return fail(status.error());
    }
    status = renderer.shutdown();
    if (!status) {
        return fail(status.error());
    }
    status = runtime.shutdown();
    if (!status) {
        return fail(status.error());
    }
    if (const auto* state = active_platform.value()->find_window(window.value());
        state != nullptr && state->open) {
        status = active_platform.value()->close_window(window.value());
        if (!status) {
            return fail(status.error());
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return heartstead::core::run_process_entry(argv[0], [argc, argv] {
        const auto parsed_options = parse_options(argc, argv);
        if (!parsed_options) {
            print_usage(argv[0], std::cerr);
            std::cerr << parsed_options.error().code << ": " << parsed_options.error().message
                      << '\n';
            return 2;
        }
        const auto options = parsed_options.value();
        if (options.help) {
            print_usage(argv[0], std::cout);
            return 0;
        }
        const auto content_report = heartstead::content::ContentValidation::validate(
            std::filesystem::path(HEARTSTEAD_SOURCE_ROOT));
        if (content_report.has_errors()) {
            return fail("content validation failed");
        }
        auto runtime = create_runtime(content_report);
        if (!runtime) {
            return fail(runtime.error());
        }
        auto status = start_runtime(runtime.value(), content_report, options);
        if (!status) {
            return fail(status.error());
        }
        if (options.headless) {
            return run_headless(runtime.value(), options.maximum_frames.value_or(120));
        }
        return run_native(runtime.value(), content_report, options);
    });
}
