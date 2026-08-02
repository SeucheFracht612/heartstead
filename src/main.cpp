#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "heartstead/app/menu_controller.hpp"
#include "heartstead/app/video_settings_controller.hpp"
#include "heartstead/app/world_editor.hpp"
#include "heartstead/app/world_save_store.hpp"
#include "heartstead/core/math.hpp"
#include "heartstead/game/interaction.hpp"
#include "heartstead/game/player.hpp"
#include "heartstead/platform/input.hpp"
#include "heartstead/platform/window.hpp"
#include "heartstead/render/opengl_renderer.hpp"
#include "heartstead/world/chunk_world.hpp"
#include "heartstead/world/world_generation.hpp"
#include "heartstead/world/world_streamer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

enum class SettingsReturnTarget { gameplay, main_menu, pause };

using heartstead::game::Camera;
using heartstead::game::CameraMode;
using heartstead::game::MouseLookState;
using heartstead::game::Player;
using heartstead::game::block_intersects_player;
using heartstead::game::interaction_raycast;
using heartstead::game::surface_height;
using heartstead::game::toggle_flight;
using heartstead::game::update_camera_pose;
using heartstead::game::update_player;
using heartstead::game::update_view_angles;
using heartstead::platform::WindowedPlacement;
using heartstead::platform::InputRouter;
using heartstead::platform::apply_fullscreen;
using heartstead::platform::process_cpu_seconds;
using heartstead::world::WorldArea;
using heartstead::world::WorldEdits;
using heartstead::world::SceneBuildResult;
using heartstead::world::StreamBatch;
using heartstead::world::build_scene;
using heartstead::world::build_stream_batch;
using heartstead::world::desired_world_center;

void glfw_error(int code, const char* description) {
    std::cerr << "GLFW error " << code << ": " << description << '\n';
}

} // namespace

int main() {
    using Clock = std::chrono::steady_clock;
    using namespace heartstead;

    glfwSetErrorCallback(glfw_error);
    if (glfwInit() != GLFW_TRUE) {
        std::cerr << "Could not initialize the window system.\n";
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_SAMPLES, 4);

    auto* window = glfwCreateWindow(1280, 720, "Heartstead | Loading voxel world...", nullptr, nullptr);
    if (window == nullptr) {
        std::cerr << "Could not create an OpenGL 3.3 window. Update the graphics driver and try again.\n";
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 800, 600, GLFW_DONT_CARE, GLFW_DONT_CARE);

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }

    int result = 0;
    try {
        InputRouter input(window);
        app::MenuController menu_controller;
        app::VideoSettingsController video_settings_controller;
        app::WorldSaveStore save_store;
        Camera camera;
        Player player;
        CameraMode camera_mode = CameraMode::first_person;
        VideoSettings active_settings;
        VideoSettings pending_settings = active_settings;
        WindowedPlacement windowed_placement;
        glfwGetWindowPos(window, &windowed_placement.x, &windowed_placement.y);
        glfwGetWindowSize(window, &windowed_placement.width, &windowed_placement.height);
        windowed_placement.valid = true;
        WorldArea area;
        WorldEdits world_edits;
        ChunkWorld world;
        std::unique_ptr<OpenGlRenderer> renderer;
        std::uint32_t loaded_quad_count = 0;
        std::size_t loaded_vertex_count = 0;

        const auto install_scene = [&](SceneBuildResult&& scene) {
            area = scene.area;
            world = std::move(scene.world);
            loaded_quad_count = scene.mesh.quad_count;
            loaded_vertex_count = scene.mesh.vertices.size();
            renderer = std::make_unique<OpenGlRenderer>(scene.mesh, Float3{
                static_cast<float>(area.center_chunk.x * Chunk::edge), 0.0F,
                static_cast<float>(area.center_chunk.z * Chunk::edge)});
            std::cout << active_settings.render_distance_chunks << "x"
                      << active_settings.render_distance_chunks << " chunks, "
                      << loaded_quad_count << " quads, " << loaded_vertex_count << " vertices, "
                      << scene.milliseconds << " ms generation + meshing\n";
        };
        char loading_title[128]{};
        std::snprintf(loading_title, sizeof(loading_title),
            "Heartstead | Generating %dx%d voxel world...",
            active_settings.render_distance_chunks, active_settings.render_distance_chunks);
        glfwSetWindowTitle(window, loading_title);
        glfwPollEvents();
        std::uint64_t scene_revision = 1;
        install_scene(build_scene(
            desired_world_center(camera, active_settings), active_settings, world_edits, scene_revision));
        player.position.y = surface_height(world, player.position.x, player.position.z);
        update_camera_pose(camera, player, world, camera_mode);
        std::future<SceneBuildResult> full_scene_build;
        std::future<StreamBatch> incremental_build;
        std::optional<StreamBatch> pending_stream;
        std::size_t pending_upload_index = 0;
        std::vector<ChunkMeshUpdate> upload_slice;
        upload_slice.reserve(24);
        bool force_stream_rebuild = false;

        auto previous_time = Clock::now();
        auto title_time = previous_time;
        auto metrics_time = previous_time;
        auto metrics_cpu_time = process_cpu_seconds();
        std::uint32_t title_frames = 0;
        std::uint32_t metrics_frames = 0;
        bool game_started = false;
        bool menu_visible = true;
        bool pause_open = false;
        bool settings_open = false;
        SettingsReturnTarget settings_return = SettingsReturnTarget::main_menu;
        bool debug_open = false;
        DebugStats debug_stats;
        double smoothed_cpu_frame_milliseconds = 0.0;
        VideoSettingsUiState settings_ui;
        MenuUiState menu_ui;
        MenuUiState pause_ui;
        pause_ui.screen = MenuScreen::pause;
        std::string active_world_name;
        std::optional<app::WorldSaveData> active_save;
        bool previous_mouse_left = false;
        bool previous_break_mouse = false;
        bool previous_place_mouse = false;
        bool reset_camera_mouse = true;
        MouseLookState mouse_look;
        auto last_autosave = Clock::now();

        const auto refresh_saved_worlds = [&] {
            menu_ui.saved_worlds.clear();
            for (const auto& summary : save_store.list()) {
                menu_ui.saved_worlds.push_back({
                    .id = summary.id,
                    .name = summary.name,
                    .created = summary.created,
                    .last_played = summary.last_played,
                });
            }
            if (menu_ui.saved_worlds.empty()) {
                menu_ui.selected_world = -1;
            } else {
                menu_ui.selected_world = 0;
            }
        };
        const auto save_active_world = [&] {
            if (!active_save) return;
            active_save->edits = world_edits;
            active_save->player_position = player.position;
            active_save->player_yaw = player.yaw;
            active_save->camera_yaw = camera.yaw;
            active_save->camera_pitch = camera.pitch;
            if (!save_store.save(*active_save))
                std::cerr << "Could not save world '" << active_save->summary.name << "'.\n";
            last_autosave = Clock::now();
        };
        refresh_saved_worlds();

        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        if (glfwRawMouseMotionSupported() == GLFW_TRUE)
            glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        glfwSetWindowTitle(window, "Heartstead | Main Menu");

        const auto open_settings = [&](SettingsReturnTarget return_target) {
            settings_open = true;
            settings_return = return_target;
            menu_visible = false;
            pending_settings = active_settings;
            settings_ui = {};
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            if (glfwRawMouseMotionSupported() == GLFW_TRUE)
                glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
            glfwSetWindowTitle(window, "Heartstead | Video Settings");
        };
        const auto close_settings = [&] {
            const auto return_target = settings_return;
            const auto regenerate =
                pending_settings.render_distance_chunks != active_settings.render_distance_chunks;
            const auto vsync_changed = pending_settings.vsync != active_settings.vsync;
            const auto fullscreen_changed = pending_settings.fullscreen != active_settings.fullscreen;
            active_settings = pending_settings;
            if (fullscreen_changed)
                apply_fullscreen(window, active_settings.fullscreen, windowed_placement);
            if (vsync_changed) glfwSwapInterval(active_settings.vsync ? 1 : 0);
            settings_open = false;
            menu_visible = return_target == SettingsReturnTarget::main_menu;
            pause_open = return_target == SettingsReturnTarget::pause;
            const auto show_cursor = menu_visible || pause_open;
            glfwSetInputMode(window, GLFW_CURSOR,
                show_cursor ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);
            if (glfwRawMouseMotionSupported() == GLFW_TRUE)
                glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION,
                    show_cursor ? GLFW_FALSE : GLFW_TRUE);
            reset_camera_mouse = true;
            if (regenerate) {
                ++scene_revision;
                force_stream_rebuild = true;
            }
            glfwSetWindowTitle(window,
                menu_visible ? "Heartstead | Main Menu"
                             : (pause_open ? "Heartstead | Paused" : "Heartstead"));
            title_time = Clock::now();
            title_frames = 0;
        };
        const auto start_world = [&](app::WorldSaveData data, bool newly_created) {
            active_save = std::move(data);
            active_world_name = active_save->summary.name;
            world_edits = active_save->edits;
            player = Player{};
            player.position = active_save->player_position;
            player.yaw = active_save->player_yaw;
            camera = Camera{};
            camera.yaw = active_save->camera_yaw;
            camera.pitch = active_save->camera_pitch;
            camera_mode = CameraMode::third_person;
            update_camera_pose(camera, player, world, camera_mode);
            ++scene_revision;
            install_scene(build_scene(
                desired_world_center(camera, active_settings),
                active_settings, world_edits, scene_revision));
            if (newly_created)
                player.position.y = surface_height(world, player.position.x, player.position.z);
            update_camera_pose(camera, player, world, camera_mode);
            pending_stream.reset();
            pending_upload_index = 0;
            force_stream_rebuild = false;
            game_started = true;
            menu_visible = false;
            pause_open = false;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            if (glfwRawMouseMotionSupported() == GLFW_TRUE)
                glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
            reset_camera_mouse = true;
            title_time = Clock::now();
            title_frames = 0;
            last_autosave = Clock::now();
            if (newly_created) save_active_world();
        };

        std::cout << "Heartstead voxel demo started.\n"
                  << "WASD move | mouse look | Space jump/up | Ctrl fly down | Shift sprint | "
                     "F fly | Left break | Right place | F3 debug | F5 camera | Escape pause\n"
                  << world.chunk_count() << " chunks, " << loaded_quad_count << " quads, "
                  << loaded_vertex_count << " vertices\n"
                  << "World memory: " << world.memory_bytes() << " bytes\n";

        while (glfwWindowShouldClose(window) == GLFW_FALSE) {
            const auto cpu_frame_start = Clock::now();
            glfwPollEvents();
            const auto input_events = input.consume_events();
            bool pause_opened_this_frame = false;
            if (game_started && !settings_open && !pause_open && input_events.debug_toggle) {
                debug_open = !debug_open;
            }
            if (game_started && !settings_open && !pause_open && input_events.view_toggle) {
                camera_mode = camera_mode == CameraMode::first_person
                    ? CameraMode::third_person
                    : CameraMode::first_person;
                update_camera_pose(camera, player, world, camera_mode);
            }
            if (game_started && !settings_open && !pause_open && input_events.flight_toggle) {
                toggle_flight(player);
                std::cout << "Flight mode "
                          << (player.movement_mode == game::MovementMode::flying ? "enabled" : "disabled")
                          << ".\n";
            }
            if (settings_open && input_events.settings_toggle) {
                if (settings_open && settings_ui.editing_render_distance) {
                    settings_ui.editing_render_distance = false;
                    settings_ui.numeric_text.clear();
                } else {
                    close_settings();
                }
            } else if (game_started && !menu_visible && !settings_open &&
                !pause_open && input_events.settings_toggle) {
                pause_open = true;
                pause_opened_this_frame = true;
                pause_ui.hovered_control = -1;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                if (glfwRawMouseMotionSupported() == GLFW_TRUE)
                    glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
                glfwSetWindowTitle(window, "Heartstead | Paused");
            }

            const auto now = Clock::now();
            const auto delta = std::min(std::chrono::duration<float>(now - previous_time).count(), 0.1F);
            previous_time = now;
            double mouse_x = 0.0;
            double mouse_y = 0.0;
            glfwGetCursorPos(window, &mouse_x, &mouse_y);

            std::int32_t width = 0;
            std::int32_t height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            if (settings_open) {
                const auto mouse_left = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                video_settings_controller.update(window, pending_settings, settings_ui, width, height,
                    mouse_left && !previous_mouse_left, input_events);
                previous_mouse_left = mouse_left;
                previous_break_mouse = mouse_left;
                previous_place_mouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
            } else if (menu_visible) {
                const auto mouse_left = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                const auto menu_action = menu_controller.update(window, menu_ui, width, height,
                    mouse_left && !previous_mouse_left, input_events);
                previous_mouse_left = mouse_left;
                previous_break_mouse = mouse_left;
                previous_place_mouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                if (menu_action == app::MenuAction::open_settings) {
                    open_settings(SettingsReturnTarget::main_menu);
                } else if (menu_action == app::MenuAction::refresh_worlds) {
                    refresh_saved_worlds();
                } else if (menu_action == app::MenuAction::play_world) {
                    const auto selected = static_cast<std::size_t>(menu_ui.selected_world);
                    if (selected < menu_ui.saved_worlds.size()) {
                        const auto loaded = save_store.load(menu_ui.saved_worlds[selected].id);
                        if (loaded) start_world(std::move(*loaded), false);
                    }
                } else if (menu_action == app::MenuAction::create_world) {
                    auto created = save_store.create(menu_ui.world_name, menu_ui.creation_date);
                    start_world(std::move(created), true);
                    std::cout << "Created world '" << active_world_name
                              << "' on " << menu_ui.creation_date << ".\n";
                } else if (menu_action == app::MenuAction::quit) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            } else if (pause_open) {
                const auto mouse_left = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                auto pause_input_events = input_events;
                if (pause_opened_this_frame)
                    pause_input_events.settings_toggle = false;
                const auto pause_action = menu_controller.update(window, pause_ui, width, height,
                    mouse_left && !previous_mouse_left, pause_input_events);
                previous_mouse_left = mouse_left;
                previous_break_mouse = mouse_left;
                previous_place_mouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                if (pause_action == app::MenuAction::open_settings) {
                    open_settings(SettingsReturnTarget::pause);
                } else if (pause_action == app::MenuAction::return_main_menu) {
                    save_active_world();
                    active_save.reset();
                    game_started = false;
                    pause_open = false;
                    menu_visible = true;
                    menu_ui.screen = MenuScreen::main;
                    refresh_saved_worlds();
                    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
                    if (glfwRawMouseMotionSupported() == GLFW_TRUE)
                        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
                    glfwSetWindowTitle(window, "Heartstead | Main Menu");
                } else if (pause_action == app::MenuAction::resume) {
                    pause_open = false;
                    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                    if (glfwRawMouseMotionSupported() == GLFW_TRUE)
                        glfwSetInputMode(window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
                    reset_camera_mouse = true;
                }
            } else {
                previous_mouse_left = false;
                update_view_angles(camera, mouse_look, mouse_x, mouse_y, reset_camera_mouse);
                reset_camera_mouse = false;
                update_player(player, camera, world, input.player_input(), delta, camera_mode);
                update_camera_pose(camera, player, world, camera_mode);

                const auto break_mouse =
                    glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
                const auto place_mouse =
                    glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
                if ((break_mouse && !previous_break_mouse) || (place_mouse && !previous_place_mouse)) {
                    const auto hit = interaction_raycast(world, camera, player, camera_mode);
                    if (hit) {
                        bool edited = false;
                        bool uploaded = true;
                        if (break_mouse && !previous_break_mouse) {
                            uploaded = app::WorldEditor::set_block(
                                world, world_edits, *renderer, hit->block, air_block);
                            edited = true;
                        } else if (place_mouse && !previous_place_mouse &&
                            world.get_block(hit->adjacent) == air_block &&
                            !block_intersects_player(hit->adjacent, player)) {
                            uploaded = app::WorldEditor::set_block(
                                world, world_edits, *renderer, hit->adjacent, 1);
                            edited = true;
                        }
                        if (edited) {
                            ++scene_revision;
                            if (!uploaded) force_stream_rebuild = true;
                        }
                    }
                }
                previous_break_mouse = break_mouse;
                previous_place_mouse = place_mouse;

                const auto desired_center = desired_world_center(camera, active_settings);
                if (full_scene_build.valid() &&
                    full_scene_build.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    auto scene = full_scene_build.get();
                    const auto target_error = std::max(
                        std::abs(scene.area.center_chunk.x - desired_center.x),
                        std::abs(scene.area.center_chunk.z - desired_center.z));
                    if (scene.revision == scene_revision && target_error <= 12) {
                        install_scene(std::move(scene));
                    } else {
                        force_stream_rebuild = true;
                    }
                }
                if (incremental_build.valid() &&
                    incremental_build.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    auto batch = incremental_build.get();
                    if (batch.revision == scene_revision) {
                        pending_stream = std::move(batch);
                        pending_upload_index = 0;
                    }
                }
                if (pending_stream && pending_stream->revision != scene_revision) {
                    pending_stream.reset();
                    pending_upload_index = 0;
                }
                if (pending_stream) {
                    constexpr std::size_t uploads_per_frame = 24;
                    const auto upload_end = std::min(
                        pending_upload_index + uploads_per_frame, pending_stream->mesh_updates.size());
                    upload_slice.clear();
                    for (auto index = pending_upload_index; index < upload_end; ++index)
                        upload_slice.push_back(std::move(pending_stream->mesh_updates[index]));
                    if (!renderer->apply_chunk_updates(upload_slice, {})) {
                        pending_stream.reset();
                        pending_upload_index = 0;
                        force_stream_rebuild = true;
                    } else {
                        pending_upload_index = upload_end;
                        if (pending_upload_index == pending_stream->mesh_updates.size()) {
                            if (renderer->apply_chunk_updates({}, pending_stream->mesh_removals)) {
                                for (const auto coordinate : pending_stream->storage_removals)
                                    world.erase_chunk(coordinate);
                                for (auto& update : pending_stream->storage_updates)
                                    world.set_chunk(update.coordinates, std::move(update.chunk));
                                area = pending_stream->area;
                                std::cout << "Streamed " << pending_stream->mesh_updates.size()
                                          << " chunks in " << pending_stream->milliseconds << " ms\n";
                            } else {
                                force_stream_rebuild = true;
                            }
                            pending_stream.reset();
                            pending_upload_index = 0;
                        }
                    }
                }
                if (force_stream_rebuild && !full_scene_build.valid()) {
                    const auto settings_snapshot = active_settings;
                    const auto edits_snapshot = world_edits;
                    const auto revision_snapshot = scene_revision;
                    full_scene_build = std::async(std::launch::async,
                        [desired_center, settings_snapshot, edits_snapshot, revision_snapshot] {
                            return build_scene(
                                desired_center, settings_snapshot, edits_snapshot, revision_snapshot);
                        });
                    force_stream_rebuild = false;
                } else if (!full_scene_build.valid() && !incremental_build.valid() && !pending_stream) {
                    const auto center_error = std::max(
                        std::abs(area.center_chunk.x - desired_center.x),
                        std::abs(area.center_chunk.z - desired_center.z));
                    if (center_error >= 2) {
                        const auto old_area = area;
                        const auto settings_snapshot = active_settings;
                        const auto edits_snapshot = world_edits;
                        const auto revision_snapshot = scene_revision;
                        incremental_build = std::async(std::launch::async,
                            [old_area, desired_center, settings_snapshot, edits_snapshot, revision_snapshot] {
                                return build_stream_batch(
                                    old_area, desired_center, settings_snapshot, edits_snapshot, revision_snapshot);
                            });
                    }
                }
            }
            if (game_started && active_save &&
                std::chrono::duration<double>(Clock::now() - last_autosave).count() >= 10.0) {
                save_active_world();
            }
            if (width > 0 && height > 0) {
                constexpr float radians_per_degree = 0.01745329252F;
                const auto panorama_visible = menu_visible ||
                    (settings_open && settings_return == SettingsReturnTarget::main_menu);
                auto render_camera = camera;
                auto render_player_position = player.position;
                if (panorama_visible) {
                    const auto panorama_time = static_cast<float>(glfwGetTime());
                    render_camera.position = {
                        static_cast<float>(area.center_chunk.x * Chunk::edge),
                        54.0F,
                        static_cast<float>(area.center_chunk.z * Chunk::edge),
                    };
                    render_camera.yaw = -1.15F + panorama_time * 0.035F;
                    render_camera.pitch = -0.24F + std::sin(panorama_time * 0.08F) * 0.025F;
                    render_player_position = {
                        render_camera.position.x, -1000.0F, render_camera.position.z};
                }
                const auto far_plane = std::max(512.0F,
                    static_cast<float>(active_settings.render_distance_chunks * Chunk::edge) * 0.9F);
                const auto projection = Matrix4::perspective(
                    70.0F * radians_per_degree,
                    static_cast<float>(width) / static_cast<float>(height),
                    0.1F, far_plane);
                const auto forward = render_camera.forward();
                const auto view = Matrix4::look_at(
                    render_camera.position, render_camera.position + forward, {0.0F, 1.0F, 0.0F});
                auto frame_settings = active_settings;
                if (settings_open) {
                    frame_settings.distance_smoothing_start = pending_settings.distance_smoothing_start;
                    frame_settings.fog_start_fraction = pending_settings.fog_start_fraction;
                    frame_settings.shadow_distance_blocks = pending_settings.shadow_distance_blocks;
                }
                renderer->render(
                    projection * view, render_camera.position, frame_settings, width, height,
                    !panorama_visible && camera_mode == CameraMode::third_person,
                    game_started && !settings_open && !pause_open,
                    render_player_position, player.yaw);
                if (menu_visible) renderer->render_menu(menu_ui, active_settings, width, height);
                if (pause_open && !settings_open)
                    renderer->render_menu(pause_ui, active_settings, width, height);
                if (settings_open) renderer->render_video_settings(pending_settings, settings_ui, width, height);
                if (game_started && !settings_open && !pause_open && debug_open)
                    renderer->render_debug_overlay(debug_stats, width, height);
            }

            const auto cpu_frame_milliseconds =
                std::chrono::duration<double, std::milli>(Clock::now() - cpu_frame_start).count();
            smoothed_cpu_frame_milliseconds = smoothed_cpu_frame_milliseconds == 0.0
                ? cpu_frame_milliseconds
                : smoothed_cpu_frame_milliseconds * 0.90 + cpu_frame_milliseconds * 0.10;
            if (width > 0 && height > 0) glfwSwapBuffers(window);

            ++metrics_frames;
            const auto metrics_now = Clock::now();
            const auto metrics_elapsed = std::chrono::duration<double>(metrics_now - metrics_time).count();
            if (metrics_elapsed >= 0.5) {
                const auto current_cpu_time = process_cpu_seconds();
                const auto process_cpu_elapsed = current_cpu_time - metrics_cpu_time;
                const auto processor_count = std::max(1U, std::thread::hardware_concurrency());
                debug_stats.frames_per_second = static_cast<double>(metrics_frames) / metrics_elapsed;
                debug_stats.cpu_frame_milliseconds = smoothed_cpu_frame_milliseconds;
                debug_stats.cpu_usage_percent = std::clamp(
                    process_cpu_elapsed / metrics_elapsed * 100.0 / static_cast<double>(processor_count),
                    0.0, 999.0);
                debug_stats.gpu_frame_milliseconds = renderer->gpu_frame_milliseconds();
                const auto frame_budget_milliseconds = 1000.0 /
                    std::max(1.0, debug_stats.frames_per_second);
                debug_stats.gpu_load_percent = std::clamp(
                    debug_stats.gpu_frame_milliseconds / frame_budget_milliseconds * 100.0,
                    0.0, 999.0);
                debug_stats.world_storage_bytes = world.memory_bytes();
                debug_stats.gpu_storage_bytes = renderer->gpu_storage_bytes();
                debug_stats.visible_chunks = renderer->visible_chunk_count();
                debug_stats.occluded_chunks = renderer->occluded_chunk_count();
                debug_stats.total_chunks = renderer->total_chunk_count();
                metrics_time = metrics_now;
                metrics_cpu_time = current_cpu_time;
                metrics_frames = 0;
            }

            ++title_frames;
            const auto title_elapsed = std::chrono::duration<double>(now - title_time).count();
            if (game_started && !settings_open && !pause_open && title_elapsed >= 0.5) {
                const auto fps = static_cast<double>(title_frames) / title_elapsed;
                char title[256]{};
                std::snprintf(title, sizeof(title),
                    "Heartstead | %s | %s | %s | %dx%d | %u visible | %u GPU-occluded | %u total | %.0f FPS%s | F fly | F3 debug | F5 view | ESC menu",
                    active_world_name.c_str(),
                    camera_mode == CameraMode::first_person ? "First person" : "Valheim view",
                    player.movement_mode == game::MovementMode::flying ? "Flying" : "Walking",
                    active_settings.render_distance_chunks, active_settings.render_distance_chunks,
                    renderer->visible_chunk_count(), renderer->occluded_chunk_count(),
                    renderer->total_chunk_count(), fps,
                    (full_scene_build.valid() || incremental_build.valid() || pending_stream)
                        ? " | streaming" : "");
                glfwSetWindowTitle(window, title);
                title_time = now;
                title_frames = 0;
            }
        }
        save_active_world();
    } catch (const std::exception& error) {
        std::cerr << "Heartstead could not start: " << error.what() << '\n';
        result = 1;
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return result;
}
