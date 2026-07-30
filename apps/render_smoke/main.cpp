#include "engine/core/logging.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/platform/platform.hpp"
#include "engine/renderer/renderer.hpp"
#include "engine/renderer/shaders/spirv_loader.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace heartstead;

constexpr world::ChunkCoord test_world_center{1'000'000'000, 0, -1'000'000'000};

[[nodiscard]] core::Status populate_test_world(world::WorldState& state) {
    constexpr auto edge = static_cast<std::int64_t>(world::VoxelChunk::edge_length);
    for (std::int64_t chunk_z = -1; chunk_z <= 1; ++chunk_z) {
        for (std::int64_t chunk_x = -1; chunk_x <= 1; ++chunk_x) {
            const world::ChunkCoord coord{test_world_center.x + chunk_x, test_world_center.y,
                                          test_world_center.z + chunk_z};
            auto& chunk = state.chunks().get_or_create(coord);
            for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
                for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
                    const auto terrain_x = static_cast<float>(chunk_x * edge + x);
                    const auto terrain_z = static_cast<float>(chunk_z * edge + z);
                    const auto wave =
                        3.0F * std::sin(terrain_x * 0.12F) + 2.0F * std::cos(terrain_z * 0.15F);
                    const auto height = static_cast<std::uint16_t>(
                        std::clamp(7 + static_cast<int>(std::lround(wave)), 2, 14));
                    for (std::uint16_t y = 0; y <= height; ++y) {
                        const std::uint16_t type = y == height ? 1 : (y + 3 >= height ? 2 : 3);
                        auto status = chunk.set({x, y, z}, world::VoxelCell{type, 255, 0, 0});
                        if (!status) {
                            return status;
                        }
                    }
                }
            }
        }
    }
    return core::Status::ok();
}

[[nodiscard]] int fail(std::string_view message) {
    core::log(core::LogLevel::error, message);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    return heartstead::core::run_no_argument_process_entry(argc, argv, [] {
        using namespace heartstead;
        using namespace renderer::rhi;

        core::log(core::LogLevel::info, "Heartstead retained terrain renderer starting");
        const auto native_info = platform::platform_backend_info(platform::PlatformBackend::native);
        if (!native_info.available) {
            return fail("A native X11 display is required: " + std::string(native_info.status));
        }
        const auto vulkan_info = renderer_backend_info(RenderBackend::vulkan);
        if (!vulkan_info.available) {
            return fail("A Vulkan graphics device is required: " + std::string(vulkan_info.status));
        }

        auto active_platform = platform::create_platform({platform::PlatformBackend::native});
        if (!active_platform) {
            return fail(active_platform.error().message);
        }
        constexpr RenderExtent initial_extent{1280, 720};
        auto window = active_platform.value()->create_window(
            {"Heartstead - Retained Chunk Renderer", initial_extent.width, initial_extent.height,
             true});
        if (!window) {
            return fail(window.error().message);
        }
        auto native_handle = active_platform.value()->native_window_handle(window.value());
        if (!native_handle) {
            return fail("Native platform did not expose a Vulkan window handle");
        }

        RenderDeviceDesc device_desc;
        device_desc.backend = RenderBackend::vulkan;
        device_desc.application_name = "Heartstead Retained Terrain Renderer";
        device_desc.initial_extent = initial_extent;
        device_desc.present_mode = PresentMode::fifo;
        device_desc.enable_validation = true;
        device_desc.native_window = native_handle.value();
        auto device = create_render_device(device_desc);
        if (!device) {
            return fail(device.error().message);
        }
        core::log(
            core::LogLevel::info,
            "Vulkan validation active: " +
                std::string(device.value()->capabilities().supports_validation ? "yes" : "no"));

        const std::filesystem::path shader_root =
            std::filesystem::path{HEARTSTEAD_RENDER_SMOKE_ASSET_DIR} / "shaders";
        auto sky_vertex_spirv = renderer::shaders::load_spirv_file(shader_root / "sky.vert.spv");
        auto sky_fragment_spirv = renderer::shaders::load_spirv_file(shader_root / "sky.frag.spv");
        auto vertex_spirv = renderer::shaders::load_spirv_file(shader_root / "terrain.vert.spv");
        auto fragment_spirv = renderer::shaders::load_spirv_file(shader_root / "terrain.frag.spv");
        auto static_vertex_spirv =
            renderer::shaders::load_spirv_file(shader_root / "static_mesh.vert.spv");
        auto static_fragment_spirv =
            renderer::shaders::load_spirv_file(shader_root / "static_mesh.frag.spv");
        auto shadow_terrain_fragment_spirv =
            renderer::shaders::load_spirv_file(shader_root / "shadow_terrain.frag.spv");
        auto shadow_static_fragment_spirv =
            renderer::shaders::load_spirv_file(shader_root / "shadow_static.frag.spv");
        auto debug_vertex_spirv =
            renderer::shaders::load_spirv_file(shader_root / "debug_line.vert.spv");
        auto debug_fragment_spirv =
            renderer::shaders::load_spirv_file(shader_root / "debug_line.frag.spv");
        auto ui_vertex_spirv = renderer::shaders::load_spirv_file(shader_root / "ui.vert.spv");
        auto ui_fragment_spirv = renderer::shaders::load_spirv_file(shader_root / "ui.frag.spv");
        auto tone_map_vertex_spirv =
            renderer::shaders::load_spirv_file(shader_root / "tone_map.vert.spv");
        auto tone_map_fragment_spirv =
            renderer::shaders::load_spirv_file(shader_root / "tone_map.frag.spv");
        auto ssao_fragment_spirv =
            renderer::shaders::load_spirv_file(shader_root / "ssao.frag.spv");
        auto ao_composite_fragment_spirv =
            renderer::shaders::load_spirv_file(shader_root / "ao_composite.frag.spv");
        auto fxaa_fragment_spirv =
            renderer::shaders::load_spirv_file(shader_root / "fxaa.frag.spv");
        auto bloom_fragment_spirv =
            renderer::shaders::load_spirv_file(shader_root / "bloom.frag.spv");
        if (!sky_vertex_spirv || !sky_fragment_spirv) {
            return fail("Sky shader loading failed visibly: " +
                        (!sky_vertex_spirv ? sky_vertex_spirv.error().message
                                           : sky_fragment_spirv.error().message));
        }
        if (!vertex_spirv) {
            return fail("Vertex shader loading failed visibly: " + vertex_spirv.error().message);
        }
        if (!fragment_spirv) {
            return fail("Fragment shader loading failed visibly: " +
                        fragment_spirv.error().message);
        }
        if (!static_vertex_spirv || !static_fragment_spirv || !shadow_terrain_fragment_spirv ||
            !shadow_static_fragment_spirv) {
            return fail("Static-mesh shader loading failed visibly: " +
                        (!static_vertex_spirv     ? static_vertex_spirv.error().message
                         : !static_fragment_spirv ? static_fragment_spirv.error().message
                         : !shadow_terrain_fragment_spirv
                             ? shadow_terrain_fragment_spirv.error().message
                             : shadow_static_fragment_spirv.error().message));
        }
        if (!debug_vertex_spirv || !debug_fragment_spirv) {
            return fail("Debug shader loading failed visibly: " +
                        (!debug_vertex_spirv ? debug_vertex_spirv.error().message
                                             : debug_fragment_spirv.error().message));
        }
        if (!ui_vertex_spirv || !ui_fragment_spirv) {
            return fail("UI shader loading failed visibly: " +
                        (!ui_vertex_spirv ? ui_vertex_spirv.error().message
                                          : ui_fragment_spirv.error().message));
        }
        if (!tone_map_vertex_spirv || !tone_map_fragment_spirv || !ssao_fragment_spirv ||
            !ao_composite_fragment_spirv || !fxaa_fragment_spirv || !bloom_fragment_spirv) {
            return fail("Tone map shader loading failed visibly: " +
                        (!tone_map_vertex_spirv     ? tone_map_vertex_spirv.error().message
                         : !tone_map_fragment_spirv ? tone_map_fragment_spirv.error().message
                         : !ssao_fragment_spirv     ? ssao_fragment_spirv.error().message
                         : !ao_composite_fragment_spirv
                             ? ao_composite_fragment_spirv.error().message
                         : !fxaa_fragment_spirv ? fxaa_fragment_spirv.error().message
                                                : bloom_fragment_spirv.error().message));
        }

        renderer::RendererInitDesc renderer_init;
        renderer_init.device = std::move(device).value();
        renderer_init.sky_vertex_spirv = std::move(sky_vertex_spirv).value();
        renderer_init.sky_fragment_spirv = std::move(sky_fragment_spirv).value();
        renderer_init.terrain_vertex_spirv = std::move(vertex_spirv).value();
        renderer_init.terrain_fragment_spirv = std::move(fragment_spirv).value();
        renderer_init.static_mesh_vertex_spirv = std::move(static_vertex_spirv).value();
        renderer_init.static_mesh_fragment_spirv = std::move(static_fragment_spirv).value();
        renderer_init.shadow_terrain_fragment_spirv =
            std::move(shadow_terrain_fragment_spirv).value();
        renderer_init.shadow_static_fragment_spirv =
            std::move(shadow_static_fragment_spirv).value();
        renderer_init.debug_vertex_spirv = std::move(debug_vertex_spirv).value();
        renderer_init.debug_fragment_spirv = std::move(debug_fragment_spirv).value();
        renderer_init.ui_vertex_spirv = std::move(ui_vertex_spirv).value();
        renderer_init.ui_fragment_spirv = std::move(ui_fragment_spirv).value();
        renderer_init.tone_map_vertex_spirv = std::move(tone_map_vertex_spirv).value();
        renderer_init.tone_map_fragment_spirv = std::move(tone_map_fragment_spirv).value();
        renderer_init.ssao_fragment_spirv = std::move(ssao_fragment_spirv).value();
        renderer_init.ao_composite_fragment_spirv = std::move(ao_composite_fragment_spirv).value();
        renderer_init.fxaa_fragment_spirv = std::move(fxaa_fragment_spirv).value();
        renderer_init.bloom_fragment_spirv = std::move(bloom_fragment_spirv).value();
        // Exposure and tone curve are overridable so the resolve pass can be observed doing its
        // job: a pipeline that ignored its push constants would not respond to either.
        if (const auto* stops = std::getenv("HEARTSTEAD_EXPOSURE"); stops != nullptr) {
            renderer_init.exposure.exposure_stops = std::strtof(stops, nullptr);
        }
        if (const auto* curve = std::getenv("HEARTSTEAD_TONE_MAP"); curve != nullptr) {
            const std::string_view name{curve};
            renderer_init.exposure.tone_mapping =
                name == "none"          ? renderer::rhi::RenderToneMapping::none
                : name == "reinhard"    ? renderer::rhi::RenderToneMapping::reinhard
                : name == "pbr_neutral" ? renderer::rhi::RenderToneMapping::khronos_pbr_neutral
                                        : renderer::rhi::RenderToneMapping::aces_approx;
        }
        // Log the resolved scale, not just the stops: a brightness that disagrees with this
        // number means the constants are not reaching the resolve pass.
        const auto exposure_constants =
            renderer::rhi::make_tone_map_push_constants(renderer_init.exposure);
        core::log(core::LogLevel::info,
                  "Exposure: " + std::to_string(renderer_init.exposure.exposure_stops) +
                      " stops (scale x" + std::to_string(exposure_constants.exposure_scale) +
                      "), tone map: " +
                      std::string(renderer::rhi::render_tone_mapping_name(
                          renderer_init.exposure.tone_mapping)));
        renderer_init.chunk_config.max_chunks_meshed_per_frame = 2;
        renderer_init.chunk_config.max_bytes_uploaded_per_frame = 4 * 1024 * 1024;
        renderer::Renderer retained_renderer;
        auto renderer_status = retained_renderer.initialize(std::move(renderer_init));
        if (!renderer_status) {
            return fail(renderer_status.error().message);
        }

        world::WorldState world;
        auto populate_status = populate_test_world(world);
        if (!populate_status) {
            return fail(populate_status.error().message);
        }
        auto center_origin = world::chunk_local_to_block(test_world_center, {0, 0, 0});
        if (!center_origin) {
            return fail(center_origin.error().message);
        }

        renderer::RenderCamera camera;
        camera.floating_origin.block = {center_origin.value().x + 16, center_origin.value().y,
                                        center_origin.value().z + 96};
        camera.local_position = {0.0F, 22.0F, 0.0F};
        camera.pitch_radians = -0.28F;
        auto camera_status = camera.set_aspect_ratio(static_cast<float>(initial_extent.width) /
                                                     static_cast<float>(initial_extent.height));
        if (!camera_status) {
            return fail(camera_status.error().message);
        }

        core::log(core::LogLevel::info,
                  "Nine terrain chunks queued. Controls: WASD move, Space rises, hold right mouse "
                  "to look, Esc exits.");
        RenderExtent framebuffer_extent = initial_extent;
        auto previous_time = std::chrono::steady_clock::now();
        bool have_last_mouse = false;
        std::int32_t last_mouse_x = 0;
        std::int32_t last_mouse_y = 0;
        std::uint64_t rendered_frames = 0;
        while (!active_platform.value()->should_quit()) {
            active_platform.value()->begin_frame();
            while (auto event = active_platform.value()->poll_event()) {
                if (event->kind == platform::PlatformEventKind::quit_requested ||
                    event->kind == platform::PlatformEventKind::window_closed) {
                    active_platform.value()->request_quit();
                } else if (event->kind == platform::PlatformEventKind::window_resized &&
                           event->window_id == window.value()) {
                    framebuffer_extent = {event->width, event->height};
                    if (framebuffer_extent.is_valid()) {
                        renderer_status = retained_renderer.resize(framebuffer_extent);
                        if (!renderer_status) {
                            return fail(renderer_status.error().message);
                        }
                        camera_status =
                            camera.set_aspect_ratio(static_cast<float>(framebuffer_extent.width) /
                                                    static_cast<float>(framebuffer_extent.height));
                        if (!camera_status) {
                            return fail(camera_status.error().message);
                        }
                        core::log(core::LogLevel::info,
                                  "Renderer resized to " +
                                      std::to_string(framebuffer_extent.width) + "x" +
                                      std::to_string(framebuffer_extent.height));
                    }
                }
            }
            if (active_platform.value()->should_quit()) {
                break;
            }
            if (active_platform.value()->was_key_pressed(window.value(),
                                                         platform::KeyCode::escape)) {
                active_platform.value()->request_quit();
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration<float>(now - previous_time).count();
            previous_time = now;
            const auto delta_seconds = std::clamp(elapsed, 0.0F, 0.1F);
            constexpr float movement_speed = 18.0F;
            auto planar_forward = camera.forward();
            planar_forward.y = 0.0F;
            const auto planar_length = static_cast<float>(math::length(planar_forward));
            if (planar_length > 0.0F) {
                planar_forward /= planar_length;
            }
            if (active_platform.value()->is_key_down(window.value(), platform::KeyCode::w)) {
                camera.local_position += planar_forward * (movement_speed * delta_seconds);
            }
            if (active_platform.value()->is_key_down(window.value(), platform::KeyCode::s)) {
                camera.local_position -= planar_forward * (movement_speed * delta_seconds);
            }
            if (active_platform.value()->is_key_down(window.value(), platform::KeyCode::d)) {
                camera.local_position += camera.right() * (movement_speed * delta_seconds);
            }
            if (active_platform.value()->is_key_down(window.value(), platform::KeyCode::a)) {
                camera.local_position -= camera.right() * (movement_speed * delta_seconds);
            }
            if (active_platform.value()->is_key_down(window.value(), platform::KeyCode::space)) {
                camera.local_position.y += movement_speed * delta_seconds;
            }

            const auto input = active_platform.value()->input_snapshot(window.value());
            if (input && active_platform.value()->is_mouse_button_down(
                             window.value(), platform::MouseButton::right)) {
                if (have_last_mouse) {
                    constexpr float look_sensitivity = 0.004F;
                    camera.yaw_radians +=
                        static_cast<float>(input->mouse.x - last_mouse_x) * look_sensitivity;
                    camera.pitch_radians = std::clamp(
                        camera.pitch_radians -
                            static_cast<float>(input->mouse.y - last_mouse_y) * look_sensitivity,
                        -1.45F, 1.45F);
                }
                have_last_mouse = true;
                last_mouse_x = input->mouse.x;
                last_mouse_y = input->mouse.y;
            } else {
                have_last_mouse = false;
            }

            if (!framebuffer_extent.is_valid()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            camera_status = camera.update_matrices();
            if (!camera_status) {
                return fail(camera_status.error().message);
            }
            renderer_status = retained_renderer.synchronize_chunks(world, camera);
            if (!renderer_status) {
                return fail(renderer_status.error().message);
            }
            auto frame = retained_renderer.render_frame({camera, 1.0F, delta_seconds});
            if (!frame) {
                return fail(frame.error().message);
            }
            ++rendered_frames;
            if (rendered_frames <= 3 || rendered_frames % 180 == 0) {
                core::log(core::LogLevel::info,
                          "Retained frame " + std::to_string(rendered_frames) + ": " +
                              renderer::format_renderer_stats(retained_renderer.stats()));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        core::log(core::LogLevel::info, "Stopping retained renderer and releasing GPU resources");
        renderer_status = retained_renderer.shutdown();
        if (!renderer_status) {
            return fail(renderer_status.error().message);
        }
        if (const auto* state = active_platform.value()->find_window(window.value());
            state != nullptr && state->open) {
            auto close_status = active_platform.value()->close_window(window.value());
            if (!close_status) {
                return fail(close_status.error().message);
            }
        }
        core::log(core::LogLevel::info, "Heartstead retained terrain renderer shut down cleanly");
        return 0;
    });
}
