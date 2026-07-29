#include "engine/core/logging.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/platform/platform.hpp"
#include "engine/renderer/benchmark/benchmark_scene.hpp"
#include "engine/renderer/benchmark/benchmark_statistics.hpp"
#include "engine/renderer/particles/particle_system.hpp"
#include "engine/renderer/renderer.hpp"
#include "engine/renderer/shaders/spirv_loader.hpp"
#include "engine/world/fluids/chunk_fluid_system.hpp"
#include "engine/world/lighting/chunk_light_system.hpp"
#include "game/presentation/particle_presentation.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace heartstead;

enum class OutputFormat {
    json,
    csv,
};

struct Options {
    renderer::rhi::RenderBackend backend = renderer::rhi::RenderBackend::headless;
    renderer::benchmark::BenchmarkSceneKind scene =
        renderer::benchmark::BenchmarkSceneKind::flat_terrain;
    std::uint64_t seed = 0x485354454144ULL;
    std::uint64_t warmup_frames = 60;
    std::uint64_t measured_frames = 300;
    std::uint32_t chunk_radius = 1;
    std::uint32_t frame_cap = 0;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    OutputFormat format = OutputFormat::json;
    std::filesystem::path output;
    bool validation = true;
    bool reference_mesher = false;
    bool help = false;
    bool list_scenes = false;
};

[[nodiscard]] int fail(std::string_view message) {
    core::log(core::LogLevel::error, message);
    return 1;
}

[[nodiscard]] core::Status populate_instanced_forest_props(renderer::Renderer& active_renderer,
                                                           const renderer::RenderCamera& camera) {
    auto anchor = world::WorldPosition::from_anchor(camera.floating_origin.block, {});
    if (!anchor) {
        return core::Status::failure(anchor.error().code, anchor.error().message);
    }
    const auto center = camera.local_position + camera.forward() * 24.0F;
    const auto right = camera.right();
    for (std::uint32_t row = 0; row < 8; ++row) {
        for (std::uint32_t column = 0; column < 8; ++column) {
            renderer::RenderObjectProxy object;
            object.anchor = anchor.value();
            object.previous_transform.position =
                center + right * ((static_cast<float>(column) - 3.5F) * 2.0F) +
                math::Vec3f{0.0F, (static_cast<float>(row) - 3.5F) * 1.25F, 0.0F};
            object.current_transform = object.previous_transform;
            object.current_transform.rotation_degrees.y = static_cast<float>(column) * 11.25F;
            object.previous_transform.rotation_degrees = object.current_transform.rotation_degrees;
            object.mesh = active_renderer.fallback_mesh();
            object.material = active_renderer.fallback_material();
            object.local_bounds = {{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
            object.color = {0.18F + static_cast<float>(row) * 0.035F, 0.58F,
                            0.22F + static_cast<float>(column) * 0.025F, 1.0F};
            auto created = active_renderer.create_object(std::move(object));
            if (!created) {
                return core::Status::failure(created.error().code, created.error().message);
            }
        }
    }
    const auto debug_center = camera.local_position + camera.forward() * 8.0F;
    auto debug_origin = world::WorldPosition::from_anchor(camera.floating_origin.block,
                                                          {static_cast<double>(debug_center.x),
                                                           static_cast<double>(debug_center.y),
                                                           static_cast<double>(debug_center.z)});
    if (!debug_origin) {
        return core::Status::failure(debug_origin.error().code, debug_origin.error().message);
    }
    auto* debug = active_renderer.debug_renderer();
    if (debug == nullptr) {
        return core::Status::failure("renderer.benchmark_missing_debug_renderer",
                                     "forest benchmark requires the debug renderer");
    }
    auto debug_status = debug->submit_axes(debug_origin.value(), 2.0F, 3'600.0F);
    if (!debug_status) {
        return debug_status;
    }
    debug_status =
        debug->submit_aabb(debug_origin.value(), {{-4.0F, -2.0F, -4.0F}, {4.0F, 2.0F, 4.0F}},
                           {1.0F, 0.72F, 0.12F, 1.0F}, 3'600.0F);
    if (!debug_status) {
        return debug_status;
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status submit_benchmark_ui(renderer::Renderer& active_renderer) {
    auto* ui = active_renderer.ui_renderer();
    if (ui == nullptr) {
        return core::Status::failure("renderer.benchmark_missing_ui_renderer",
                                     "forest benchmark requires the UI renderer");
    }
    renderer::UiQuadDesc benchmark_badge;
    benchmark_badge.minimum_pixels = {18.0F, 18.0F};
    benchmark_badge.maximum_pixels = {286.0F, 58.0F};
    benchmark_badge.color = {0.12F, 0.32F, 0.62F, 0.82F};
    benchmark_badge.scissor_enabled = true;
    benchmark_badge.scissor = {12, 12, 300, 54};
    auto ui_status = ui->submit_quad(benchmark_badge);
    if (!ui_status) {
        return ui_status;
    }
    return ui->submit_text({{26.0F, 31.0F},
                            "HEARTSTEAD RENDERER",
                            8.0F,
                            {1.0F, 1.0F, 1.0F, 1.0F},
                            true,
                            {12, 12, 300, 54}});
}

[[nodiscard]] renderer::ParticlePrototype particle_stress_prototype() {
    renderer::ParticlePrototype prototype;
    prototype.id = *core::PrototypeId::parse("benchmark:particles/stress");
    prototype.lifetime_min_seconds = 600.0F;
    prototype.lifetime_max_seconds = 600.0F;
    prototype.speed_min = 0.0F;
    prototype.speed_max = 0.0F;
    prototype.direction_spread = 0.0F;
    prototype.gravity = 0.0F;
    prototype.drag = 0.0F;
    prototype.size_min = 0.08F;
    prototype.size_max = 0.08F;
    prototype.end_size_multiplier = 1.0F;
    prototype.start_color = {1.0F, 0.32F, 0.04F, 0.9F};
    prototype.end_color = prototype.start_color;
    return prototype;
}

void print_usage(std::ostream& output) {
    output << "Usage: heartstead_render_benchmark [options]\n"
              "  --scene NAME       flat, mountains, caves, checkerboard, forest, rapid-edits,\n"
              "                     flythrough, churn, large-coordinates, resize-minimize,\n"
              "                     active-water, particles\n"
              "  --vulkan           Use a native Vulkan window (headless is the default)\n"
              "  --headless         Use the deterministic validation backend\n"
              "  --frames N         Measured frames (default 300)\n"
              "  --warmup N         Unrecorded warm-up frames (default 60)\n"
              "  --radius N         Horizontal chunk radius, 0..8 (default 1)\n"
              "  --width N          Initial framebuffer width (default 1280)\n"
              "  --height N         Initial framebuffer height (default 720)\n"
              "  --seed N           Deterministic unsigned 64-bit scene seed\n"
              "  --frame-cap N      Sleep to cap at N FPS; 0 is uncapped (default)\n"
              "  --output PATH      Result path (default benchmark-SCENE.json)\n"
              "  --format json|csv  Result serialization format\n"
              "  --reference-mesher Use the correctness-reference terrain mesher\n"
              "  --no-validation    Do not request Vulkan validation\n"
              "  --list-scenes      Print scene names\n"
              "  --help             Print this help\n";
}

void print_scenes() {
    using Kind = renderer::benchmark::BenchmarkSceneKind;
    constexpr Kind kinds[]{
        Kind::flat_terrain,           Kind::mountainous_terrain,     Kind::dense_caves,
        Kind::checkerboard_geometry,  Kind::forest_cross_planes,     Kind::rapid_voxel_edits,
        Kind::high_speed_flythrough,  Kind::chunk_load_unload_churn, Kind::large_coordinates,
        Kind::resize_minimize_stress, Kind::active_water, Kind::particle_stress,
    };
    for (const auto kind : kinds) {
        std::cout << renderer::benchmark::benchmark_scene_name(kind) << '\n';
    }
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_unsigned(std::string_view text) noexcept {
    Integer value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] core::Result<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const auto next_value = [&]() -> core::Result<std::string_view> {
            if (index + 1 >= argc) {
                return core::Result<std::string_view>::failure(
                    "renderer.benchmark_missing_argument",
                    std::string(argument) + " requires a value");
            }
            ++index;
            return core::Result<std::string_view>::success(argv[index]);
        };
        if (argument == "--help") {
            options.help = true;
            continue;
        }
        if (argument == "--list-scenes") {
            options.list_scenes = true;
            continue;
        }
        if (argument == "--vulkan") {
            options.backend = renderer::rhi::RenderBackend::vulkan;
        } else if (argument == "--headless") {
            options.backend = renderer::rhi::RenderBackend::headless;
        } else if (argument == "--no-validation") {
            options.validation = false;
        } else if (argument == "--reference-mesher") {
            options.reference_mesher = true;
        } else if (argument == "--scene") {
            auto value = next_value();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto scene = renderer::benchmark::parse_benchmark_scene(value.value());
            if (!scene) {
                return core::Result<Options>::failure("renderer.benchmark_unknown_scene",
                                                      "unknown benchmark scene: " +
                                                          std::string(value.value()));
            }
            options.scene = *scene;
        } else if (argument == "--frames" || argument == "--warmup" || argument == "--seed" ||
                   argument == "--radius" || argument == "--frame-cap" || argument == "--width" ||
                   argument == "--height") {
            auto value = next_value();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            if (argument == "--frames") {
                const auto parsed = parse_unsigned<std::uint64_t>(value.value());
                if (!parsed || *parsed == 0) {
                    return core::Result<Options>::failure("renderer.benchmark_invalid_frames",
                                                          "--frames must be greater than zero");
                }
                options.measured_frames = *parsed;
            } else if (argument == "--warmup") {
                const auto parsed = parse_unsigned<std::uint64_t>(value.value());
                if (!parsed) {
                    return core::Result<Options>::failure("renderer.benchmark_invalid_warmup",
                                                          "--warmup must be an unsigned integer");
                }
                options.warmup_frames = *parsed;
            } else if (argument == "--seed") {
                const auto parsed = parse_unsigned<std::uint64_t>(value.value());
                if (!parsed) {
                    return core::Result<Options>::failure("renderer.benchmark_invalid_seed",
                                                          "--seed must be an unsigned integer");
                }
                options.seed = *parsed;
            } else if (argument == "--radius") {
                const auto parsed = parse_unsigned<std::uint32_t>(value.value());
                if (!parsed || *parsed > 8) {
                    return core::Result<Options>::failure("renderer.benchmark_invalid_radius",
                                                          "--radius must be in the range 0..8");
                }
                options.chunk_radius = *parsed;
            } else if (argument == "--width" || argument == "--height") {
                const auto parsed = parse_unsigned<std::uint32_t>(value.value());
                if (!parsed || *parsed < 64 || *parsed > 16'384) {
                    return core::Result<Options>::failure(
                        "renderer.benchmark_invalid_extent",
                        "--width and --height must be in the range 64..16384");
                }
                if (argument == "--width") {
                    options.width = *parsed;
                } else {
                    options.height = *parsed;
                }
            } else {
                const auto parsed = parse_unsigned<std::uint32_t>(value.value());
                if (!parsed) {
                    return core::Result<Options>::failure(
                        "renderer.benchmark_invalid_frame_cap",
                        "--frame-cap must be an unsigned integer");
                }
                options.frame_cap = *parsed;
            }
        } else if (argument == "--output") {
            auto value = next_value();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.output = value.value();
        } else if (argument == "--format") {
            auto value = next_value();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            if (value.value() == "json") {
                options.format = OutputFormat::json;
            } else if (value.value() == "csv") {
                options.format = OutputFormat::csv;
            } else {
                return core::Result<Options>::failure("renderer.benchmark_invalid_format",
                                                      "--format must be json or csv");
            }
        } else {
            return core::Result<Options>::failure("renderer.benchmark_unknown_option",
                                                  "unknown option: " + std::string(argument));
        }
    }
    if (options.output.empty()) {
        options.output = "benchmark-" +
                         std::string(renderer::benchmark::benchmark_scene_name(options.scene)) +
                         (options.format == OutputFormat::json ? ".json" : ".csv");
    }
    return core::Result<Options>::success(options);
}

struct NativeWindow {
    std::unique_ptr<platform::IPlatform> platform;
    platform::WindowId id;
};

[[nodiscard]] core::Result<NativeWindow> create_native_window(renderer::rhi::RenderExtent extent) {
    auto active_platform = platform::create_platform({platform::PlatformBackend::native});
    if (!active_platform) {
        return core::Result<NativeWindow>::failure(active_platform.error().code,
                                                   active_platform.error().message);
    }
    auto window = active_platform.value()->create_window(
        {"Heartstead Renderer Benchmark", extent.width, extent.height, true});
    if (!window) {
        return core::Result<NativeWindow>::failure(window.error().code, window.error().message);
    }
    return core::Result<NativeWindow>::success(
        NativeWindow{std::move(active_platform).value(), window.value()});
}

[[nodiscard]] core::Result<bool> pump_native_events(NativeWindow& window,
                                                    renderer::Renderer& active_renderer,
                                                    renderer::RenderCamera& camera) {
    window.platform->begin_frame();
    while (auto event = window.platform->poll_event()) {
        if (event->kind == platform::PlatformEventKind::quit_requested ||
            event->kind == platform::PlatformEventKind::window_closed) {
            window.platform->request_quit();
        } else if (event->kind == platform::PlatformEventKind::window_resized &&
                   event->window_id == window.id && event->width != 0 && event->height != 0) {
            const renderer::rhi::RenderExtent extent{event->width, event->height};
            auto status = active_renderer.resize(extent);
            if (!status) {
                return core::Result<bool>::failure(status.error().code, status.error().message);
            }
            status = camera.set_aspect_ratio(static_cast<float>(extent.width) /
                                             static_cast<float>(extent.height));
            if (!status) {
                return core::Result<bool>::failure(status.error().code, status.error().message);
            }
        }
    }
    return core::Result<bool>::success(!window.platform->should_quit());
}

[[nodiscard]] core::Result<world::ChunkLightSystemStats>
settle_chunk_lighting(world::ChunkLightSystem& lighting, world::WorldState& world,
                      const world::VoxelPalette& palette) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::size_t maximum_backlog = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        auto status = lighting.update(world.chunks(), world.dirty_regions(), palette);
        if (!status) {
            return core::Result<world::ChunkLightSystemStats>::failure(status.error().code,
                                                                       status.error().message);
        }
        auto stats = lighting.stats();
        maximum_backlog = std::max(maximum_backlog, stats.snapshot_pending_cell_count);
        if (!stats.relight_requested && !stats.snapshot_in_progress && !stats.solve_in_flight &&
            stats.completed_mailbox_count == 0) {
            stats.snapshot_pending_cell_count = maximum_backlog;
            return core::Result<world::ChunkLightSystemStats>::success(stats);
        }
        if (stats.solve_in_flight) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        } else {
            std::this_thread::yield();
        }
    }
    return core::Result<world::ChunkLightSystemStats>::failure(
        "render_benchmark.relight_settlement_timeout",
        "chunk lighting did not settle within the benchmark update budget");
}

} // namespace

int main(int argc, char** argv) {
    return heartstead::core::run_process_entry(argv[0], [argc, argv] {
        using namespace heartstead;
        const auto parsed_options = parse_options(argc, argv);
        if (!parsed_options) {
            print_usage(std::cerr);
            std::cerr << parsed_options.error().code << ": " << parsed_options.error().message
                      << '\n';
            return 2;
        }
        const auto& options = parsed_options.value();
        if (options.help) {
            print_usage(std::cout);
            return 0;
        }
        if (options.list_scenes) {
            print_scenes();
            return 0;
        }
        const renderer::rhi::RenderExtent initial_extent{options.width, options.height};

        renderer::benchmark::BenchmarkSceneConfig scene_config;
        scene_config.kind = options.scene;
        scene_config.seed = options.seed;
        scene_config.chunk_radius = options.chunk_radius;
        scene_config.initial_extent = initial_extent;
        auto scene = renderer::benchmark::BenchmarkScene::create(scene_config);
        if (!scene) {
            return fail(scene.error().message);
        }

        std::optional<NativeWindow> native_window;
        std::optional<platform::NativeWindowHandle> native_handle;
        if (options.backend == renderer::rhi::RenderBackend::vulkan) {
            auto window = create_native_window(initial_extent);
            if (!window) {
                return fail(window.error().message);
            }
            native_window = std::move(window).value();
            native_handle = native_window->platform->native_window_handle(native_window->id);
            if (!native_handle) {
                return fail("native platform did not expose a Vulkan window handle");
            }
        }

        renderer::rhi::RenderDeviceDesc device_desc;
        device_desc.backend = options.backend;
        device_desc.application_name = "Heartstead Renderer Benchmark";
        device_desc.initial_extent = initial_extent;
        device_desc.present_mode = renderer::rhi::PresentMode::immediate;
        device_desc.enable_validation = options.validation;
        device_desc.native_window = native_handle;
        auto device = renderer::rhi::create_render_device(device_desc);
        if (!device) {
            return fail(device.error().message);
        }

        std::vector<std::uint32_t> sky_vertex_spirv;
        std::vector<std::uint32_t> sky_fragment_spirv;
        std::vector<std::uint32_t> vertex_spirv;
        std::vector<std::uint32_t> fragment_spirv;
        std::vector<std::uint32_t> static_vertex_spirv;
        std::vector<std::uint32_t> static_fragment_spirv;
        std::vector<std::uint32_t> debug_vertex_spirv;
        std::vector<std::uint32_t> debug_fragment_spirv;
        std::vector<std::uint32_t> ui_vertex_spirv;
        std::vector<std::uint32_t> ui_fragment_spirv;
        if (options.backend == renderer::rhi::RenderBackend::vulkan) {
            const auto shader_root =
                std::filesystem::path{HEARTSTEAD_RENDER_BENCHMARK_ASSET_DIR} / "shaders";
            auto sky_vertex = renderer::shaders::load_spirv_file(shader_root / "sky.vert.spv");
            auto sky_fragment = renderer::shaders::load_spirv_file(shader_root / "sky.frag.spv");
            auto vertex = renderer::shaders::load_spirv_file(shader_root / "terrain.vert.spv");
            auto fragment = renderer::shaders::load_spirv_file(shader_root / "terrain.frag.spv");
            auto static_vertex =
                renderer::shaders::load_spirv_file(shader_root / "static_mesh.vert.spv");
            auto static_fragment =
                renderer::shaders::load_spirv_file(shader_root / "static_mesh.frag.spv");
            auto debug_vertex =
                renderer::shaders::load_spirv_file(shader_root / "debug_line.vert.spv");
            auto debug_fragment =
                renderer::shaders::load_spirv_file(shader_root / "debug_line.frag.spv");
            auto ui_vertex = renderer::shaders::load_spirv_file(shader_root / "ui.vert.spv");
            auto ui_fragment = renderer::shaders::load_spirv_file(shader_root / "ui.frag.spv");
            if (!sky_vertex || !sky_fragment || !vertex || !fragment || !static_vertex ||
                !static_fragment || !debug_vertex || !debug_fragment || !ui_vertex ||
                !ui_fragment) {
                const auto& error = !sky_vertex        ? sky_vertex.error()
                                    : !sky_fragment    ? sky_fragment.error()
                                    : !vertex          ? vertex.error()
                                    : !fragment        ? fragment.error()
                                    : !static_vertex   ? static_vertex.error()
                                    : !static_fragment ? static_fragment.error()
                                    : !debug_vertex    ? debug_vertex.error()
                                    : !debug_fragment  ? debug_fragment.error()
                                    : !ui_vertex       ? ui_vertex.error()
                                                       : ui_fragment.error();
                return fail(error.message);
            }
            sky_vertex_spirv = std::move(sky_vertex).value();
            sky_fragment_spirv = std::move(sky_fragment).value();
            vertex_spirv = std::move(vertex).value();
            fragment_spirv = std::move(fragment).value();
            static_vertex_spirv = std::move(static_vertex).value();
            static_fragment_spirv = std::move(static_fragment).value();
            debug_vertex_spirv = std::move(debug_vertex).value();
            debug_fragment_spirv = std::move(debug_fragment).value();
            ui_vertex_spirv = std::move(ui_vertex).value();
            ui_fragment_spirv = std::move(ui_fragment).value();
        } else {
            vertex_spirv = {0x07230203, 0x00010000, 0, 1, 0};
            fragment_spirv = vertex_spirv;
            sky_vertex_spirv = vertex_spirv;
            sky_fragment_spirv = vertex_spirv;
            static_vertex_spirv = vertex_spirv;
            static_fragment_spirv = vertex_spirv;
            debug_vertex_spirv = vertex_spirv;
            debug_fragment_spirv = vertex_spirv;
            ui_vertex_spirv = vertex_spirv;
            ui_fragment_spirv = vertex_spirv;
        }

        renderer::RendererInitDesc renderer_init;
        renderer_init.device = std::move(device).value();
        renderer_init.sky_vertex_spirv = std::move(sky_vertex_spirv);
        renderer_init.sky_fragment_spirv = std::move(sky_fragment_spirv);
        renderer_init.terrain_vertex_spirv = std::move(vertex_spirv);
        renderer_init.terrain_fragment_spirv = std::move(fragment_spirv);
        renderer_init.static_mesh_vertex_spirv = std::move(static_vertex_spirv);
        renderer_init.static_mesh_fragment_spirv = std::move(static_fragment_spirv);
        renderer_init.debug_vertex_spirv = std::move(debug_vertex_spirv);
        renderer_init.debug_fragment_spirv = std::move(debug_fragment_spirv);
        renderer_init.ui_vertex_spirv = std::move(ui_vertex_spirv);
        renderer_init.ui_fragment_spirv = std::move(ui_fragment_spirv);
        renderer_init.voxel_palette = &scene.value()->palette();
        renderer_init.chunk_config.max_chunks_meshed_per_frame = 64;
        renderer_init.chunk_config.max_bytes_uploaded_per_frame = 512U * 1024U * 1024U;
        renderer_init.chunk_config.distances.mesh_horizontal_radius = 22;
        renderer_init.chunk_config.distances.gpu_resident_horizontal_radius = 22;
        renderer_init.chunk_config.meshing_mode = options.reference_mesher
                                                      ? renderer::ChunkMeshingMode::reference
                                                      : renderer::ChunkMeshingMode::greedy;
        renderer::Renderer active_renderer;
        auto status = active_renderer.initialize(std::move(renderer_init));
        if (!status) {
            return fail(status.error().message);
        }
        auto chunk_lighting = world::ChunkLightSystem::create(scene.value()->palette());
        if (!chunk_lighting) {
            return fail(chunk_lighting.error().message);
        }
        auto initial_lighting = settle_chunk_lighting(
            *chunk_lighting.value(), scene.value()->world(), scene.value()->palette());
        if (!initial_lighting) {
            return fail(initial_lighting.error().message);
        }
        active_renderer.set_voxel_lighting_stats(initial_lighting.value());
        auto chunk_fluids = world::ChunkFluidSystem::create(scene.value()->palette());
        if (!chunk_fluids) {
            return fail(chunk_fluids.error().message);
        }
        std::optional<renderer::CpuParticleSystem> particle_system;
        game::ParticlePresentation particle_presentation;
        if (options.scene == renderer::benchmark::BenchmarkSceneKind::particle_stress) {
            renderer::ParticleSystemConfig particle_config;
            particle_config.maximum_particles = 50'000;
            particle_config.maximum_emitters = 1;
            particle_config.maximum_queued_events = 1;
            particle_config.maximum_spawns_per_update = 50'000;
            const std::array prototypes{particle_stress_prototype()};
            auto created_particles =
                renderer::CpuParticleSystem::create(particle_config, prototypes);
            if (!created_particles) {
                return fail(created_particles.error().message);
            }
            particle_system.emplace(std::move(created_particles).value());
            auto particle_origin = world::WorldPosition::from_anchor(
                scene.value()->camera().floating_origin.block,
                math::Vec3d{0.0, 24.0, 0.0} +
                    math::Vec3d{
                        static_cast<double>(scene.value()->camera().forward().x),
                        static_cast<double>(scene.value()->camera().forward().y),
                        static_cast<double>(scene.value()->camera().forward().z)} *
                        24.0);
            if (!particle_origin) {
                return fail(particle_origin.error().message);
            }
            status = particle_system->queue_event(
                {prototypes.front().id, particle_origin.value(), {0.0F, 1.0F, 0.0F}, {},
                 50'000, options.seed == 0 ? 1 : options.seed});
            if (!status) {
                return fail(status.error().message);
            }
            status = particle_system->update(1.0F / 60.0F);
            if (!status) {
                return fail(status.error().message);
            }
            status = particle_presentation.initialize(active_renderer);
            if (!status) {
                return fail(status.error().message);
            }
            auto presented = particle_presentation.synchronize(
                active_renderer, *particle_system, scene.value()->camera());
            if (!presented) {
                return fail(presented.error().message);
            }
            active_renderer.set_particle_stats(
                particle_system->stats(), presented.value().synchronize_ms,
                presented.value().material_groups, presented.value().dropped_particles);
        }
        if (options.scene == renderer::benchmark::BenchmarkSceneKind::forest_cross_planes) {
            status = populate_instanced_forest_props(active_renderer, scene.value()->camera());
            if (!status) {
                return fail(status.error().message);
            }
        }

        const auto initial_chunk_count = scene.value()->world().chunks().identities().size();
        std::size_t settlement_frames = 0;
        constexpr std::size_t maximum_settlement_frames = 10'000;
        bool settled = false;
        while (settlement_frames < maximum_settlement_frames) {
            if (native_window) {
                auto keep_running =
                    pump_native_events(*native_window, active_renderer, scene.value()->camera());
                if (!keep_running) {
                    return fail(keep_running.error().message);
                }
                if (!keep_running.value()) {
                    return fail("benchmark window closed during initial renderer settlement");
                }
            }
            status =
                active_renderer.synchronize_chunks(scene.value()->world(), scene.value()->camera());
            if (!status) {
                return fail(status.error().message);
            }
            if (options.scene == renderer::benchmark::BenchmarkSceneKind::forest_cross_planes) {
                status = submit_benchmark_ui(active_renderer);
                if (!status) {
                    return fail(status.error().message);
                }
            }
            auto frame = active_renderer.render_frame({scene.value()->camera()});
            if (!frame) {
                return fail(frame.error().message);
            }
            ++settlement_frames;
            const auto& chunks = active_renderer.chunk_stats();
            if (chunks.cache.resident_chunk_count == initial_chunk_count &&
                chunks.pending_mesh_count == 0 && chunks.pending_upload_count == 0) {
                settled = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!settled) {
            return fail(
                "initial benchmark chunks did not become resident within the settlement budget");
        }

        const auto scene_name =
            std::string(renderer::benchmark::benchmark_scene_name(options.scene));
        renderer::benchmark::BenchmarkRunMetadata benchmark_metadata;
        benchmark_metadata.scene = scene_name;
        benchmark_metadata.seed = options.seed;
        benchmark_metadata.backend = renderer::rhi::render_backend_name(options.backend);
        benchmark_metadata.mesher = options.reference_mesher ? "reference" : "greedy";
        benchmark_metadata.initial_width = initial_extent.width;
        benchmark_metadata.initial_height = initial_extent.height;
        benchmark_metadata.chunk_radius = options.chunk_radius;
        benchmark_metadata.warmup_frames = options.warmup_frames;
        benchmark_metadata.measured_frames = options.measured_frames;
        benchmark_metadata.frame_cap = options.frame_cap;
        benchmark_metadata.validation_requested = options.validation;
        renderer::benchmark::BenchmarkRecorder recorder(std::move(benchmark_metadata));
        core::log(
            core::LogLevel::info,
            "Benchmark " + scene_name + " starting: " + std::to_string(options.warmup_frames) +
                " warm-up, " + std::to_string(options.measured_frames) + " measured, " +
                (options.frame_cap == 0 ? "uncapped" : std::to_string(options.frame_cap) + " FPS") +
                ", backend=" + std::string(renderer::rhi::render_backend_name(options.backend)) +
                ", mesher=" + (options.reference_mesher ? "reference" : "greedy") +
                ", settled=" + std::to_string(settlement_frames) + " frames");

        std::uint64_t simulation_frame = 0;
        std::uint64_t rendered_frames = 0;
        std::uint64_t measured_frames = 0;
        while (measured_frames < options.measured_frames) {
            const auto frame_started = std::chrono::steady_clock::now();
            if (native_window) {
                auto keep_running =
                    pump_native_events(*native_window, active_renderer, scene.value()->camera());
                if (!keep_running) {
                    return fail(keep_running.error().message);
                }
                if (!keep_running.value()) {
                    break;
                }
            }
            auto step = scene.value()->advance(simulation_frame++);
            if (!step) {
                return fail(step.error().message);
            }
            scene.value()->activate_fluid_work(*chunk_fluids.value());
            status = chunk_fluids.value()->update(
                scene.value()->world().chunks(), scene.value()->world().dirty_regions(),
                scene.value()->palette(), simulation_frame);
            if (!status) {
                return fail(status.error().message);
            }
            active_renderer.set_voxel_fluid_stats(chunk_fluids.value()->stats());
            if (particle_system.has_value()) {
                status = particle_system->update(1.0F / 60.0F);
                if (!status) {
                    return fail(status.error().message);
                }
                auto presented = particle_presentation.synchronize(
                    active_renderer, *particle_system, scene.value()->camera());
                if (!presented) {
                    return fail(presented.error().message);
                }
                active_renderer.set_particle_stats(
                    particle_system->stats(), presented.value().synchronize_ms,
                    presented.value().material_groups, presented.value().dropped_particles);
            }
            auto lighting = settle_chunk_lighting(*chunk_lighting.value(), scene.value()->world(),
                                                  scene.value()->palette());
            if (!lighting) {
                return fail(lighting.error().message);
            }
            active_renderer.set_voxel_lighting_stats(lighting.value());
            if (step.value().requested_extent && step.value().requested_extent->is_valid()) {
                const auto extent = *step.value().requested_extent;
                status = active_renderer.resize(extent);
                if (!status) {
                    return fail(status.error().message);
                }
                status = scene.value()->camera().set_aspect_ratio(
                    static_cast<float>(extent.width) / static_cast<float>(extent.height));
                if (!status) {
                    return fail(status.error().message);
                }
            }
            if (!step.value().skip_render) {
                status = active_renderer.synchronize_chunks(scene.value()->world(),
                                                            scene.value()->camera());
                if (!status) {
                    return fail(status.error().message);
                }
                if (options.scene == renderer::benchmark::BenchmarkSceneKind::forest_cross_planes) {
                    status = submit_benchmark_ui(active_renderer);
                    if (!status) {
                        return fail(status.error().message);
                    }
                }
                auto frame = active_renderer.render_frame({scene.value()->camera()});
                if (!frame) {
                    return fail(frame.error().message);
                }
                ++rendered_frames;
                if (rendered_frames > options.warmup_frames) {
                    recorder.record(active_renderer.stats());
                    ++measured_frames;
                }
                if (rendered_frames <= 3 || rendered_frames % 120 == 0) {
                    core::log(core::LogLevel::info,
                              renderer::format_renderer_stats(active_renderer.stats()));
                }
            }
            if (options.frame_cap != 0) {
                const auto frame_duration =
                    std::chrono::duration<double>(1.0 / static_cast<double>(options.frame_cap));
                std::this_thread::sleep_until(
                    frame_started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                        frame_duration));
            }
        }

        chunk_lighting.value()->shutdown();
        if (particle_presentation.is_initialized()) {
            status = particle_presentation.shutdown(active_renderer);
            if (!status) {
                return fail(status.error().message);
            }
        }
        status = active_renderer.shutdown();
        if (!status) {
            return fail(status.error().message);
        }
        if (native_window && native_window->platform->find_window(native_window->id) != nullptr) {
            status = native_window->platform->close_window(native_window->id);
            if (!status) {
                return fail(status.error().message);
            }
        }
        if (measured_frames != options.measured_frames) {
            return fail("benchmark stopped before collecting the requested measured frames");
        }

        status = options.format == OutputFormat::json ? recorder.write_json(options.output)
                                                      : recorder.write_csv(options.output);
        if (!status) {
            return fail(status.error().message);
        }
        core::log(core::LogLevel::info,
                  renderer::benchmark::format_benchmark_summary(recorder.summarize()));
        core::log(core::LogLevel::info, "Wrote benchmark results to " + options.output.string());
        return 0;
    });
}
