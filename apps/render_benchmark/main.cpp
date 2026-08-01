#include "engine/content/content_validation.hpp"
#include "engine/core/file_io.hpp"
#include "engine/core/logging.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/platform/platform.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/renderer/benchmark/benchmark_scene.hpp"
#include "engine/renderer/benchmark/benchmark_statistics.hpp"
#include "engine/renderer/environment/weather_effects.hpp"
#include "engine/renderer/particles/particle_system.hpp"
#include "engine/renderer/renderer.hpp"
#include "engine/renderer/shaders/spirv_loader.hpp"
#include "engine/renderer/testing/visual_regression.hpp"
#include "engine/renderer/vegetation/vegetation_renderer.hpp"
#include "engine/renderer/water/large_water_renderer.hpp"
#include "engine/world/fluids/chunk_fluid_system.hpp"
#include "engine/world/lighting/chunk_light_system.hpp"
#include "game/presentation/particle_presentation.hpp"
#include "game/presentation/model_presentation_system.hpp"

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
    renderer::benchmark::BenchmarkBudgetProfile budget_profile =
        renderer::benchmark::BenchmarkBudgetProfile::none;
    OutputFormat format = OutputFormat::json;
    std::filesystem::path output;
    std::filesystem::path capture_output;
    std::filesystem::path compare_baseline;
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

[[nodiscard]] core::Result<world::WorldPosition>
camera_world_position(const renderer::RenderCamera& camera) {
    return world::WorldPosition::from_anchor(
        camera.floating_origin.block,
        {static_cast<double>(camera.local_position.x),
         static_cast<double>(camera.local_position.y),
         static_cast<double>(camera.local_position.z)});
}

[[nodiscard]] core::Status populate_starting_biome(
    renderer::Renderer& active_renderer,
    const content::ContentValidationReport& content,
    renderer::VegetationRenderer& vegetation,
    renderer::LargeWaterRenderer& water,
    const renderer::RenderCamera& camera, std::uint64_t seed) {
    auto status = vegetation.initialize(
        active_renderer, content.vegetation_species,
        std::filesystem::path{HEARTSTEAD_RENDER_BENCHMARK_COOKED_ASSET_DIR});
    if (!status) {
        return status;
    }
    const auto make_origin = [&camera](double x, double y, double z) {
        return world::WorldPosition::from_anchor(camera.floating_origin.block, {x, y, z});
    };
    struct PatchSpec {
        std::uint64_t id;
        std::string_view species;
        math::Vec3d offset;
        math::Vec2f extent;
        std::uint32_t count;
        std::string_view growth;
    };
    constexpr std::array patches{
        PatchSpec{1, "base:vegetation/meadow_grass", {-44.0, 9.0, -98.0},
                  {34.0F, 84.0F}, 3'200, "mature"},
        PatchSpec{2, "base:vegetation/wildflower", {-42.0, 9.1, -92.0},
                  {31.0F, 72.0F}, 620, "bloom"},
        PatchSpec{3, "base:vegetation/field_crop", {10.0, 9.2, -66.0},
                  {30.0F, 28.0F}, 1'800, "mature"},
        PatchSpec{4, "base:vegetation/temperate_tree", {-76.0, 9.0, -124.0},
                  {42.0F, 112.0F}, 220, "mature"},
        PatchSpec{5, "base:vegetation/forest_bush", {-66.0, 9.0, -112.0},
                  {38.0F, 92.0F}, 460, ""},
        PatchSpec{6, "base:vegetation/aether_bloom", {35.0, 9.0, -58.0},
                  {12.0F, 16.0F}, 96, ""},
    };
    for (const auto& spec : patches) {
        const auto species = core::PrototypeId::parse(spec.species);
        auto origin = make_origin(spec.offset.x, spec.offset.y, spec.offset.z);
        if (!species || !origin) {
            return core::Status::failure(
                "render_benchmark.invalid_starting_biome",
                "starting-biome vegetation uses an invalid prototype or world position");
        }
        renderer::VegetationPatchDesc patch;
        patch.id = spec.id;
        patch.species = *species;
        patch.origin = origin.value();
        patch.extent = spec.extent;
        patch.instance_count = spec.count;
        patch.seed = seed ^ (0x9e3779b97f4a7c15ULL * spec.id);
        if (patch.seed == 0U) {
            patch.seed = spec.id;
        }
        patch.growth_state = spec.growth;
        status = vegetation.upsert_patch(patch, [](float x, float z) {
            return std::sin(x * 0.055F) * 2.1F + std::cos(z * 0.047F) * 1.5F;
        });
        if (!status) {
            return status;
        }
    }
    status = vegetation.update_occlusion(camera, {});
    if (!status) {
        return status;
    }

    status = water.initialize(active_renderer);
    if (!status) {
        return status;
    }
    auto lake_center = make_origin(52.0, 8.65, -104.0);
    if (!lake_center) {
        return core::Status::failure(lake_center.error().code,
                                     lake_center.error().message);
    }
    renderer::LargeWaterBodyDesc lake;
    lake.id = 1;
    lake.center = lake_center.value();
    lake.half_extent = 34.0F;
    lake.wave_height = 0.08F;
    lake.wave_speed = 0.7F;
    lake.optical_depth = 7.0F;
    lake.foam_strength = 0.55F;
    lake.follows_camera = false;
    status = water.add_body(lake);
    if (!status) {
        return status;
    }

    auto fire_position = make_origin(6.0, 11.0, -28.0);
    if (!fire_position) {
        return core::Status::failure(fire_position.error().code,
                                     fire_position.error().message);
    }
    renderer::RenderLightProxy fire;
    fire.id = active_renderer.reserve_light_id();
    fire.kind = renderer::RenderLightKind::point;
    fire.anchor = fire_position.value();
    fire.color = {1.0F, 0.32F, 0.08F};
    fire.intensity = 58.0F;
    fire.radius = 16.0F;
    fire.casts_shadow = true;
    fire.gameplay_importance = 3.0F;
    auto light = active_renderer.create_light(std::move(fire));
    if (!light) {
        return core::Status::failure(light.error().code, light.error().message);
    }
    return core::Status::ok();
}

void print_usage(std::ostream& output) {
    output << "Usage: heartstead_render_benchmark [options]\n"
              "  --scene NAME       flat, mountains, caves, checkerboard, forest, rapid-edits,\n"
              "                     mass-excavation, flythrough, churn, large-coordinates,\n"
              "                     resize-minimize,\n"
              "                     active-water, particles, light-heavy, terrain-materials,\n"
              "                     starting-biome, character-workshop\n"
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
              "  --capture PATH     Write the final displayed frame as PNG plus metadata\n"
              "  --compare PATH     Compare the final frame against a baseline PNG\n"
              "  --format json|csv  Result serialization format\n"
              "  --budget PROFILE   Gate none, compatibility, minimum, mainstream, or high-end\n"
              "  --reference-mesher Use the correctness-reference terrain mesher\n"
              "  --no-validation    Do not request Vulkan validation\n"
              "  --list-scenes      Print scene names\n"
              "  --help             Print this help\n";
}

void print_scenes() {
    using Kind = renderer::benchmark::BenchmarkSceneKind;
    constexpr Kind kinds[]{
        Kind::flat_terrain,
        Kind::mountainous_terrain,
        Kind::dense_caves,
        Kind::checkerboard_geometry,
        Kind::forest_cross_planes,
        Kind::rapid_voxel_edits,
        Kind::mass_excavation,
        Kind::high_speed_flythrough,
        Kind::chunk_load_unload_churn,
        Kind::large_coordinates,
        Kind::resize_minimize_stress,
        Kind::active_water,
        Kind::particle_stress,
        Kind::light_heavy_settlement,
        Kind::terrain_material_preview,
        Kind::starting_biome,
        Kind::character_workshop,
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
        } else if (argument == "--capture" || argument == "--compare") {
            auto value = next_value();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            if (argument == "--capture") {
                options.capture_output = value.value();
            } else {
                options.compare_baseline = value.value();
            }
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
        } else if (argument == "--budget") {
            auto value = next_value();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto profile =
                renderer::benchmark::parse_benchmark_budget_profile(value.value());
            if (!profile) {
                return core::Result<Options>::failure(
                    "renderer.benchmark_invalid_budget",
                    "--budget must be none, compatibility, minimum, mainstream, or high-end");
            }
            options.budget_profile = *profile;
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
    if (options.backend == renderer::rhi::RenderBackend::headless &&
        (!options.capture_output.empty() || !options.compare_baseline.empty())) {
        return core::Result<Options>::failure(
            "renderer.benchmark_capture_requires_pixels",
            "--capture and --compare require --vulkan because headless rendering has no pixels");
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
        std::vector<std::uint32_t> far_vertex_spirv;
        std::vector<std::uint32_t> fragment_spirv;
        std::vector<std::uint32_t> static_vertex_spirv;
        std::vector<std::uint32_t> static_fragment_spirv;
        std::vector<std::uint32_t> shadow_terrain_fragment_spirv;
        std::vector<std::uint32_t> shadow_static_fragment_spirv;
        std::vector<std::uint32_t> debug_vertex_spirv;
        std::vector<std::uint32_t> debug_fragment_spirv;
        std::vector<std::uint32_t> ui_vertex_spirv;
        std::vector<std::uint32_t> ui_fragment_spirv;
        std::vector<std::uint32_t> tone_map_vertex_spirv;
        std::vector<std::uint32_t> tone_map_fragment_spirv;
        std::vector<std::uint32_t> ssao_fragment_spirv;
        std::vector<std::uint32_t> ao_composite_fragment_spirv;
        std::vector<std::uint32_t> fxaa_fragment_spirv;
        std::vector<std::uint32_t> bloom_fragment_spirv;
        if (options.backend == renderer::rhi::RenderBackend::vulkan) {
            const auto shader_root =
                std::filesystem::path{HEARTSTEAD_RENDER_BENCHMARK_ASSET_DIR} / "shaders";
            auto sky_vertex = renderer::shaders::load_spirv_file(shader_root / "sky.vert.spv");
            auto sky_fragment = renderer::shaders::load_spirv_file(shader_root / "sky.frag.spv");
            auto vertex = renderer::shaders::load_spirv_file(shader_root / "terrain.vert.spv");
            auto far_vertex =
                renderer::shaders::load_spirv_file(shader_root / "far_terrain.vert.spv");
            auto fragment = renderer::shaders::load_spirv_file(shader_root / "terrain.frag.spv");
            auto static_vertex =
                renderer::shaders::load_spirv_file(shader_root / "static_mesh.vert.spv");
            auto static_fragment =
                renderer::shaders::load_spirv_file(shader_root / "static_mesh.frag.spv");
            auto shadow_terrain_fragment =
                renderer::shaders::load_spirv_file(shader_root / "shadow_terrain.frag.spv");
            auto shadow_static_fragment =
                renderer::shaders::load_spirv_file(shader_root / "shadow_static.frag.spv");
            auto debug_vertex =
                renderer::shaders::load_spirv_file(shader_root / "debug_line.vert.spv");
            auto debug_fragment =
                renderer::shaders::load_spirv_file(shader_root / "debug_line.frag.spv");
            auto ui_vertex = renderer::shaders::load_spirv_file(shader_root / "ui.vert.spv");
            auto ui_fragment = renderer::shaders::load_spirv_file(shader_root / "ui.frag.spv");
            auto tone_map_vertex =
                renderer::shaders::load_spirv_file(shader_root / "tone_map.vert.spv");
            auto tone_map_fragment =
                renderer::shaders::load_spirv_file(shader_root / "tone_map.frag.spv");
            auto ssao_fragment = renderer::shaders::load_spirv_file(shader_root / "ssao.frag.spv");
            auto ao_composite_fragment =
                renderer::shaders::load_spirv_file(shader_root / "ao_composite.frag.spv");
            auto fxaa_fragment = renderer::shaders::load_spirv_file(shader_root / "fxaa.frag.spv");
            auto bloom_fragment =
                renderer::shaders::load_spirv_file(shader_root / "bloom.frag.spv");
            if (!sky_vertex || !sky_fragment || !vertex || !far_vertex || !fragment || !static_vertex ||
                !static_fragment || !shadow_terrain_fragment || !shadow_static_fragment ||
                !debug_vertex || !debug_fragment || !ui_vertex || !ui_fragment ||
                !tone_map_vertex || !tone_map_fragment || !ssao_fragment ||
                !ao_composite_fragment || !fxaa_fragment || !bloom_fragment) {
                const auto& error = !sky_vertex                ? sky_vertex.error()
                                    : !sky_fragment            ? sky_fragment.error()
                                    : !vertex                  ? vertex.error()
                                    : !far_vertex              ? far_vertex.error()
                                    : !fragment                ? fragment.error()
                                    : !static_vertex           ? static_vertex.error()
                                    : !static_fragment         ? static_fragment.error()
                                    : !shadow_terrain_fragment ? shadow_terrain_fragment.error()
                                    : !shadow_static_fragment  ? shadow_static_fragment.error()
                                    : !debug_vertex            ? debug_vertex.error()
                                    : !debug_fragment          ? debug_fragment.error()
                                    : !ui_vertex               ? ui_vertex.error()
                                    : !ui_fragment             ? ui_fragment.error()
                                    : !tone_map_vertex         ? tone_map_vertex.error()
                                    : !tone_map_fragment       ? tone_map_fragment.error()
                                    : !ssao_fragment           ? ssao_fragment.error()
                                    : !ao_composite_fragment   ? ao_composite_fragment.error()
                                    : !fxaa_fragment           ? fxaa_fragment.error()
                                                               : bloom_fragment.error();
                return fail(error.message);
            }
            sky_vertex_spirv = std::move(sky_vertex).value();
            sky_fragment_spirv = std::move(sky_fragment).value();
            vertex_spirv = std::move(vertex).value();
            far_vertex_spirv = std::move(far_vertex).value();
            fragment_spirv = std::move(fragment).value();
            static_vertex_spirv = std::move(static_vertex).value();
            static_fragment_spirv = std::move(static_fragment).value();
            shadow_terrain_fragment_spirv = std::move(shadow_terrain_fragment).value();
            shadow_static_fragment_spirv = std::move(shadow_static_fragment).value();
            debug_vertex_spirv = std::move(debug_vertex).value();
            debug_fragment_spirv = std::move(debug_fragment).value();
            ui_vertex_spirv = std::move(ui_vertex).value();
            ui_fragment_spirv = std::move(ui_fragment).value();
            tone_map_vertex_spirv = std::move(tone_map_vertex).value();
            tone_map_fragment_spirv = std::move(tone_map_fragment).value();
            ssao_fragment_spirv = std::move(ssao_fragment).value();
            ao_composite_fragment_spirv = std::move(ao_composite_fragment).value();
            fxaa_fragment_spirv = std::move(fxaa_fragment).value();
            bloom_fragment_spirv = std::move(bloom_fragment).value();
        } else {
            vertex_spirv = {0x07230203, 0x00010000, 0, 1, 0};
            fragment_spirv = vertex_spirv;
            far_vertex_spirv = vertex_spirv;
            sky_vertex_spirv = vertex_spirv;
            sky_fragment_spirv = vertex_spirv;
            static_vertex_spirv = vertex_spirv;
            static_fragment_spirv = vertex_spirv;
            shadow_terrain_fragment_spirv = vertex_spirv;
            shadow_static_fragment_spirv = vertex_spirv;
            debug_vertex_spirv = vertex_spirv;
            debug_fragment_spirv = vertex_spirv;
            ui_vertex_spirv = vertex_spirv;
            ui_fragment_spirv = vertex_spirv;
            tone_map_vertex_spirv = vertex_spirv;
            tone_map_fragment_spirv = vertex_spirv;
            ssao_fragment_spirv = vertex_spirv;
            ao_composite_fragment_spirv = vertex_spirv;
            fxaa_fragment_spirv = vertex_spirv;
            bloom_fragment_spirv = vertex_spirv;
        }

        renderer::RendererInitDesc renderer_init;
        auto ui_font = core::read_binary_file(
            std::filesystem::path{HEARTSTEAD_RENDER_BENCHMARK_ASSET_DIR} /
            "fonts/heartstead-ui.ttf");
        if (!ui_font) {
            return fail(ui_font.error().message);
        }
        renderer_init.device = std::move(device).value();
        renderer_init.sky_vertex_spirv = std::move(sky_vertex_spirv);
        renderer_init.sky_fragment_spirv = std::move(sky_fragment_spirv);
        renderer_init.terrain_vertex_spirv = std::move(vertex_spirv);
        renderer_init.far_terrain_vertex_spirv = std::move(far_vertex_spirv);
        renderer_init.terrain_fragment_spirv = std::move(fragment_spirv);
        renderer_init.static_mesh_vertex_spirv = std::move(static_vertex_spirv);
        renderer_init.static_mesh_fragment_spirv = std::move(static_fragment_spirv);
        renderer_init.shadow_terrain_fragment_spirv = std::move(shadow_terrain_fragment_spirv);
        renderer_init.shadow_static_fragment_spirv = std::move(shadow_static_fragment_spirv);
        renderer_init.debug_vertex_spirv = std::move(debug_vertex_spirv);
        renderer_init.debug_fragment_spirv = std::move(debug_fragment_spirv);
        renderer_init.ui_vertex_spirv = std::move(ui_vertex_spirv);
        renderer_init.ui_fragment_spirv = std::move(ui_fragment_spirv);
        renderer_init.ui_font_bytes = std::move(ui_font).value();
        renderer_init.tone_map_vertex_spirv = std::move(tone_map_vertex_spirv);
        renderer_init.tone_map_fragment_spirv = std::move(tone_map_fragment_spirv);
        renderer_init.ssao_fragment_spirv = std::move(ssao_fragment_spirv);
        renderer_init.ao_composite_fragment_spirv = std::move(ao_composite_fragment_spirv);
        renderer_init.fxaa_fragment_spirv = std::move(fxaa_fragment_spirv);
        renderer_init.bloom_fragment_spirv = std::move(bloom_fragment_spirv);
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
        std::unique_ptr<content::ContentValidationReport> environmental_content;
        renderer::VegetationRenderer vegetation;
        renderer::LargeWaterRenderer large_water;
        renderer::WeatherEffects weather_effects;
        std::optional<renderer::EvaluatedEnvironment> evaluated_environment;
        if (options.scene == renderer::benchmark::BenchmarkSceneKind::starting_biome) {
            environmental_content =
                std::make_unique<content::ContentValidationReport>(
                    content::ContentValidation::validate(
                        std::filesystem::path{HEARTSTEAD_SOURCE_ROOT}));
            if (environmental_content->has_errors()) {
                return fail("starting-biome content validation failed");
            }
            status = populate_starting_biome(
                active_renderer, *environmental_content, vegetation, large_water,
                scene.value()->camera(), options.seed);
            if (!status) {
                return fail(status.error().message);
            }
        }
        std::unique_ptr<content::ContentValidationReport> character_content;
        std::optional<game::ModelPresentationSystem> character_presentations;
        game::RenderSnapshot character_snapshot;
        if (options.scene == renderer::benchmark::BenchmarkSceneKind::character_workshop) {
            character_content = std::make_unique<content::ContentValidationReport>(
                content::ContentValidation::validate(
                    std::filesystem::path{HEARTSTEAD_SOURCE_ROOT}));
            if (character_content->has_errors()) {
                return fail("character-workshop content validation failed");
            }
            character_presentations.emplace();
            game::ModelPresentationSystemConfig presentation_config;
            presentation_config.material_registry = &character_content->material_registry;
            status = character_presentations->initialize(
                active_renderer, character_content->visual_definitions,
                std::filesystem::path{HEARTSTEAD_RENDER_BENCHMARK_COOKED_ASSET_DIR},
                presentation_config);
            if (!status) {
                return fail(status.error().message);
            }
            const auto player = core::PrototypeId::parse("base:entities/player");
            const auto workshop = core::PrototypeId::parse("base:entities/workshop_machine");
            if (!player || !workshop) {
                return fail("character-workshop prototype ids are invalid");
            }
            character_snapshot.objects.reserve(512);
            for (std::uint32_t index = 0; index < 512U; ++index) {
                const auto is_player = index < 128U;
                const auto column = static_cast<int>(index % 32U) - 16;
                const auto row = static_cast<int>(index / 32U) - 8;
                auto position = world::WorldPosition::from_anchor(
                    scene.value()->camera().floating_origin.block,
                    {static_cast<double>(column) * 2.4, 1.0,
                     static_cast<double>(row) * 2.8});
                if (!position) {
                    return fail(position.error().message);
                }
                game::RenderObjectSnapshot object;
                object.id = game::PresentationObjectId::from_parts(index, 1);
                object.source_net_id = core::NetId::from_value(index + 1U);
                object.visual_prototype = is_player ? *player : *workshop;
                object.previous_transform.position = position.value();
                object.current_transform.position = position.value();
                object.local_bounds = is_player
                                          ? math::Bounds3f{{-0.35F, 0.0F, -0.35F},
                                                           {0.35F, 1.9F, 0.35F}}
                                          : math::Bounds3f{{-1.5F, 0.0F, -1.2F},
                                                           {1.5F, 1.8F, 1.2F}};
                object.current_locomotion.kind =
                    is_player ? animation::LocomotionAnimationKind::walk
                              : animation::LocomotionAnimationKind::idle;
                object.previous_locomotion = object.current_locomotion;
                object.animation_importance = index < 32U ? 255U : 96U;
                object.source_revision = 1;
                if (is_player) {
                    object.equipment.push_back(
                        {.slot = "main_hand", .variant = "hammer", .stowed = index % 3U == 0U});
                } else {
                    object.visual_states = {
                        {.channel = "activity", .value = "active"},
                        {.channel = "process", .value = "loaded"},
                        {.channel = "heat", .value = "hot"},
                        {.channel = "access", .value = "closed"},
                        {.channel = "power", .value = "on"},
                        {.channel = "damage", .value = "intact"},
                    };
                }
                character_snapshot.objects.push_back(std::move(object));
            }
        }
        const auto synchronize_character_workshop = [&](const std::uint64_t tick) -> core::Status {
            if (!character_presentations.has_value()) {
                return core::Status::ok();
            }
            character_snapshot.simulation_tick = tick;
            ++character_snapshot.presentation_revision;
            for (auto& object : character_snapshot.objects) {
                object.source_revision = tick + 1U;
                object.previous_locomotion = object.current_locomotion;
                object.current_locomotion.phase =
                    static_cast<std::uint16_t>((tick * 997U + object.source_net_id.value() * 131U) %
                                               65'536U);
                for (auto& state : object.visual_states) {
                    if (state.channel == "activity") {
                        state.value = (tick / 120U) % 2U == 0U ? "active" : "idle";
                    } else if (state.channel == "heat") {
                        state.value = (tick / 180U) % 2U == 0U ? "hot" : "cold";
                    }
                }
            }
            auto synchronized = character_presentations->synchronize(
                active_renderer, character_snapshot, &scene.value()->camera());
            return synchronized
                       ? core::Status::ok()
                       : core::Status::failure(synchronized.error().code,
                                               synchronized.error().message);
        };
        if (options.scene == renderer::benchmark::BenchmarkSceneKind::light_heavy_settlement) {
            for (std::uint32_t index = 0; index < 128U; ++index) {
                const auto x = static_cast<double>(static_cast<int>(index % 16U) - 8) * 5.0;
                const auto z = static_cast<double>(static_cast<int>(index / 16U) - 4) * 5.0;
                auto anchor = world::WorldPosition::from_anchor(
                    scene.value()->camera().floating_origin.block, {x, 3.0, z});
                if (!anchor) {
                    return fail(anchor.error().message);
                }
                renderer::RenderLightProxy light;
                light.id = active_renderer.reserve_light_id();
                light.kind = index % 5U == 0U ? renderer::RenderLightKind::spot
                                              : renderer::RenderLightKind::point;
                light.anchor = anchor.value();
                light.direction = {0.0F, -1.0F, 0.0F};
                light.color = index % 3U == 0U ? math::Vec3f{1.0F, 0.38F, 0.12F}
                                               : math::Vec3f{0.28F, 0.55F, 1.0F};
                light.intensity = 42.0F;
                light.radius = 13.0F;
                light.casts_shadow = index < 16U;
                light.gameplay_importance = index < 8U ? 2.0F : 1.0F;
                auto created = active_renderer.create_light(std::move(light));
                if (!created) {
                    return fail(created.error().message);
                }
            }
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
                    math::Vec3d{static_cast<double>(scene.value()->camera().forward().x),
                                static_cast<double>(scene.value()->camera().forward().y),
                                static_cast<double>(scene.value()->camera().forward().z)} *
                        24.0);
            if (!particle_origin) {
                return fail(particle_origin.error().message);
            }
            status = particle_system->queue_event({prototypes.front().id,
                                                   particle_origin.value(),
                                                   {0.0F, 1.0F, 0.0F},
                                                   {},
                                                   50'000,
                                                   options.seed == 0 ? 1 : options.seed});
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
            auto presented = particle_presentation.synchronize(active_renderer, *particle_system,
                                                               scene.value()->camera());
            if (!presented) {
                return fail(presented.error().message);
            }
            active_renderer.set_particle_stats(
                particle_system->stats(), presented.value().synchronize_ms,
                presented.value().material_groups, presented.value().dropped_particles);
        } else if (options.scene ==
                   renderer::benchmark::BenchmarkSceneKind::starting_biome) {
            renderer::ParticleSystemConfig particle_config;
            particle_config.maximum_particles = 24'000;
            particle_config.maximum_emitters = 16;
            particle_config.maximum_queued_events = 2'048;
            particle_config.maximum_spawns_per_update = 4'096;
            auto created_particles = renderer::CpuParticleSystem::create(
                particle_config, environmental_content->particle_prototypes);
            if (!created_particles) {
                return fail(created_particles.error().message);
            }
            particle_system.emplace(std::move(created_particles).value());
            status = weather_effects.initialize(*particle_system);
            if (!status) {
                return fail(status.error().message);
            }
            renderer::EnvironmentBlendContext environment_context;
            environment_context.biome = "meadow";
            environment_context.weather = "rain";
            environment_context.day_fraction = 0.68F;
            environment_context.elapsed_seconds = 32.0F;
            environment_context.altitude = 12.0F;
            auto evaluated =
                environmental_content->environment_profiles.evaluate(environment_context);
            if (!evaluated) {
                return fail(evaluated.error().message);
            }
            evaluated_environment = std::move(evaluated).value();
            status = active_renderer.set_environment(evaluated_environment->render);
            if (!status) {
                return fail(status.error().message);
            }
            status = active_renderer.set_exposure(evaluated_environment->exposure);
            if (!status) {
                return fail(status.error().message);
            }
            auto viewpoint = camera_world_position(scene.value()->camera());
            if (!viewpoint) {
                return fail(viewpoint.error().message);
            }
            status = weather_effects.update(*evaluated_environment, viewpoint.value(),
                                            1.0F / 60.0F);
            if (!status) {
                return fail(status.error().message);
            }
            const auto create_emitter =
                [&](std::string_view prototype, float rate, std::uint64_t seed_offset) {
                    auto id = core::PrototypeId::parse(prototype);
                    auto position = world::WorldPosition::from_anchor(
                        scene.value()->camera().floating_origin.block,
                        {6.0, 11.0, -28.0});
                    if (!id || !position) {
                        return core::Status::failure(
                            "render_benchmark.invalid_effect_emitter",
                            "starting-biome effect emitter uses invalid data");
                    }
                    renderer::ParticleEmitterDesc emitter;
                    emitter.prototype_id = *id;
                    emitter.position = position.value();
                    emitter.direction = {0.0F, 1.0F, 0.0F};
                    emitter.lifetime_seconds = 3'600.0F;
                    emitter.rate_per_second = rate;
                    emitter.burst_count = 8;
                    emitter.seed = (options.seed ^ seed_offset) | 1U;
                    auto created = particle_system->create_emitter(emitter);
                    return created
                               ? core::Status::ok()
                               : core::Status::failure(created.error().code,
                                                       created.error().message);
                };
            status = create_emitter("base:particles/fire_ember", 22.0F, 0xE11B'3EULL);
            if (status) {
                status = create_emitter("base:particles/smoke", 8.0F, 0x5A0C'EULL);
            }
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
            status = synchronize_character_workshop(settlement_frames);
            if (!status) {
                return fail(status.error().message);
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
        benchmark_metadata.budget_profile = options.budget_profile;
        const auto runtime_metadata = profiling::query_runtime_metadata();
        benchmark_metadata.engine_version = runtime_metadata.engine_version;
        benchmark_metadata.git_commit = runtime_metadata.git_commit;
        benchmark_metadata.build_configuration = runtime_metadata.build_configuration;
        benchmark_metadata.compiler = runtime_metadata.compiler;
        benchmark_metadata.platform = runtime_metadata.platform;
        benchmark_metadata.architecture = runtime_metadata.architecture;
        benchmark_metadata.operating_system = runtime_metadata.operating_system;
        benchmark_metadata.cpu_model = runtime_metadata.cpu_model;
        benchmark_metadata.logical_cpu_count = runtime_metadata.logical_cpu_count;
        benchmark_metadata.git_dirty = runtime_metadata.git_dirty;
        benchmark_metadata.tracy_enabled = runtime_metadata.tracy_enabled;
        const auto device_info = active_renderer.device()->info();
        benchmark_metadata.gpu_name = device_info.device_name;
        benchmark_metadata.gpu_driver = device_info.driver_name;
        benchmark_metadata.gpu_driver_info = device_info.driver_info;
        benchmark_metadata.gpu_vendor_id = device_info.vendor_id;
        benchmark_metadata.gpu_device_id = device_info.device_id;
        benchmark_metadata.graphics_api_version = device_info.api_version;
        benchmark_metadata.graphics_driver_version = device_info.driver_version;
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
            status = chunk_fluids.value()->update(scene.value()->world().chunks(),
                                                  scene.value()->world().dirty_regions(),
                                                  scene.value()->palette(), simulation_frame);
            if (!status) {
                return fail(status.error().message);
            }
            active_renderer.set_voxel_fluid_stats(chunk_fluids.value()->stats());
            if (evaluated_environment.has_value()) {
                auto viewpoint = camera_world_position(scene.value()->camera());
                if (!viewpoint) {
                    return fail(viewpoint.error().message);
                }
                status = weather_effects.update(*evaluated_environment,
                                                viewpoint.value(), 1.0F / 60.0F);
                if (!status) {
                    return fail(status.error().message);
                }
                status = large_water.synchronize(scene.value()->camera());
                if (!status) {
                    return fail(status.error().message);
                }
                status = vegetation.update_occlusion(scene.value()->camera(), {});
                if (!status) {
                    return fail(status.error().message);
                }
            }
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
            status = chunk_lighting.value()->update(scene.value()->world().chunks(),
                                                    scene.value()->world().dirty_regions(),
                                                    scene.value()->palette());
            if (!status) {
                return fail(status.error().message);
            }
            active_renderer.set_voxel_lighting_stats(chunk_lighting.value()->stats());
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
                status = synchronize_character_workshop(simulation_frame);
                if (!status) {
                    return fail(status.error().message);
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

        if (!options.capture_output.empty() || !options.compare_baseline.empty()) {
            auto captured = renderer::testing::capture_output(*active_renderer.device());
            if (!captured) {
                return fail(captured.error().message);
            }
            if (!options.capture_output.empty()) {
                status = renderer::testing::write_visual_capture(options.capture_output,
                                                                  captured.value());
                if (!status) {
                    return fail(status.error().message);
                }
                core::log(core::LogLevel::info,
                          "Wrote visual capture to " + options.capture_output.string());
            }
            if (!options.compare_baseline.empty()) {
                auto baseline =
                    renderer::testing::read_visual_capture(options.compare_baseline);
                if (!baseline) {
                    return fail(baseline.error().message);
                }
                auto comparison = renderer::testing::compare_visual_captures(
                    captured.value(), baseline.value());
                if (!comparison) {
                    return fail(comparison.error().message);
                }
                core::log(core::LogLevel::info,
                          "Visual regression changed=" +
                              std::to_string(comparison.value().changed_fraction) +
                              " rmse=" + std::to_string(comparison.value().rmse) +
                              " actual=" + comparison.value().actual_hash +
                              " baseline=" + comparison.value().baseline_hash);
                if (!comparison.value().passed) {
                    return fail("visual regression thresholds exceeded");
                }
            }
        }

        chunk_lighting.value()->shutdown();
        if (particle_presentation.is_initialized()) {
            status = particle_presentation.shutdown(active_renderer);
            if (!status) {
                return fail(status.error().message);
            }
        }
        weather_effects.reset();
        if (vegetation.is_initialized()) {
            status = vegetation.shutdown();
            if (!status) {
                return fail(status.error().message);
            }
        }
        if (large_water.is_initialized()) {
            status = large_water.shutdown();
            if (!status) {
                return fail(status.error().message);
            }
        }
        if (character_presentations.has_value()) {
            status = character_presentations->shutdown(active_renderer);
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
        const auto summary = recorder.summarize();
        core::log(core::LogLevel::info, renderer::benchmark::format_benchmark_summary(summary));
        core::log(core::LogLevel::info, "Wrote benchmark results to " + options.output.string());
        if (summary.budget.evaluated && !summary.budget.passed) {
            for (const auto& violation : summary.budget.violations) {
                core::log(core::LogLevel::error,
                          "Benchmark budget violation " + violation.metric + ": actual=" +
                              std::to_string(violation.actual) +
                              " limit=" + std::to_string(violation.limit));
            }
            return 2;
        }
        return 0;
    });
}
