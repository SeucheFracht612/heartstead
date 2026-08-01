#include "game/application/game_application.hpp"

#include "engine/core/file_io.hpp"
#include "engine/core/logging.hpp"
#include "engine/renderer/shaders/spirv_loader.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>
#include <vector>

namespace heartstead::game {

namespace {

struct ApplicationShaderSet {
    std::vector<std::uint32_t> sky_vertex;
    std::vector<std::uint32_t> sky_fragment;
    std::vector<std::uint32_t> terrain_vertex;
    std::vector<std::uint32_t> far_terrain_vertex;
    std::vector<std::uint32_t> terrain_fragment;
    std::vector<std::uint32_t> static_vertex;
    std::vector<std::uint32_t> static_fragment;
    std::vector<std::uint32_t> shadow_terrain_fragment;
    std::vector<std::uint32_t> shadow_static_fragment;
    std::vector<std::uint32_t> debug_vertex;
    std::vector<std::uint32_t> debug_fragment;
    std::vector<std::uint32_t> ui_vertex;
    std::vector<std::uint32_t> ui_fragment;
    std::vector<std::uint32_t> tone_map_vertex;
    std::vector<std::uint32_t> tone_map_fragment;
    std::vector<std::uint32_t> ssao_fragment;
    std::vector<std::uint32_t> ao_composite_fragment;
    std::vector<std::uint32_t> fxaa_fragment;
    std::vector<std::uint32_t> bloom_fragment;
};

[[nodiscard]] core::Result<ApplicationShaderSet>
load_application_shaders(const std::filesystem::path& root) {
    const std::array paths{
        root / "sky.vert.spv",
        root / "sky.frag.spv",
        root / "terrain.vert.spv",
        root / "far_terrain.vert.spv",
        root / "terrain.frag.spv",
        root / "static_mesh.vert.spv",
        root / "static_mesh.frag.spv",
        root / "shadow_terrain.frag.spv",
        root / "shadow_static.frag.spv",
        root / "debug_line.vert.spv",
        root / "debug_line.frag.spv",
        root / "ui.vert.spv",
        root / "ui.frag.spv",
        root / "tone_map.vert.spv",
        root / "tone_map.frag.spv",
        root / "ssao.frag.spv",
        root / "ao_composite.frag.spv",
        root / "fxaa.frag.spv",
        root / "bloom.frag.spv",
    };
    std::array<core::Result<std::vector<std::uint32_t>>, 19> loaded{
        renderer::shaders::load_spirv_file(paths[0]),
        renderer::shaders::load_spirv_file(paths[1]),
        renderer::shaders::load_spirv_file(paths[2]),
        renderer::shaders::load_spirv_file(paths[3]),
        renderer::shaders::load_spirv_file(paths[4]),
        renderer::shaders::load_spirv_file(paths[5]),
        renderer::shaders::load_spirv_file(paths[6]),
        renderer::shaders::load_spirv_file(paths[7]),
        renderer::shaders::load_spirv_file(paths[8]),
        renderer::shaders::load_spirv_file(paths[9]),
        renderer::shaders::load_spirv_file(paths[10]),
        renderer::shaders::load_spirv_file(paths[11]),
        renderer::shaders::load_spirv_file(paths[12]),
        renderer::shaders::load_spirv_file(paths[13]),
        renderer::shaders::load_spirv_file(paths[14]),
        renderer::shaders::load_spirv_file(paths[15]),
        renderer::shaders::load_spirv_file(paths[16]),
        renderer::shaders::load_spirv_file(paths[17]),
        renderer::shaders::load_spirv_file(paths[18]),
    };
    constexpr std::array required{
        true,  true,  true,  false, true,  true,  true,  false, false, true,
        true,  true,  true,  true,  true,  false, false, false, false,
    };
    for (std::size_t index = 0; index < loaded.size(); ++index) {
        if (!loaded[index] && required[index]) {
            return core::Result<ApplicationShaderSet>::failure(
                loaded[index].error().code,
                "failed to load " + paths[index].string() + ": " + loaded[index].error().message);
        }
        if (!loaded[index]) {
            core::log(core::LogLevel::warning,
                      "optional renderer feature disabled because " + paths[index].string() +
                          " could not be loaded: " + loaded[index].error().message);
        }
    }

    const auto take = [&loaded](std::size_t index) {
        return loaded[index] ? std::move(loaded[index]).value() : std::vector<std::uint32_t>{};
    };

    ApplicationShaderSet result;
    result.sky_vertex = take(0);
    result.sky_fragment = take(1);
    result.terrain_vertex = take(2);
    result.far_terrain_vertex = take(3);
    result.terrain_fragment = take(4);
    result.static_vertex = take(5);
    result.static_fragment = take(6);
    result.shadow_terrain_fragment = take(7);
    result.shadow_static_fragment = take(8);
    result.debug_vertex = take(9);
    result.debug_fragment = take(10);
    result.ui_vertex = take(11);
    result.ui_fragment = take(12);
    result.tone_map_vertex = take(13);
    result.tone_map_fragment = take(14);
    result.ssao_fragment = take(15);
    result.ao_composite_fragment = take(16);
    result.fxaa_fragment = take(17);
    result.bloom_fragment = take(18);
    return core::Result<ApplicationShaderSet>::success(std::move(result));
}

} // namespace

core::Status GameApplicationConfig::validate() const {
    if (maximum_frames.has_value() && *maximum_frames == 0) {
        return core::Status::failure("game_application.invalid_frame_limit",
                                     "application frame limit must be greater than zero");
    }
    if (maximum_frame_delta_microseconds == 0 || maximum_frame_delta_microseconds > 10'000'000) {
        return core::Status::failure(
            "game_application.invalid_frame_delta_limit",
            "maximum frame delta must be between one microsecond and ten seconds");
    }
    if (headless) {
        if (application_worker_count == 0) {
            return core::Status::failure("game_application.invalid_worker_count",
                                         "application worker count must be non-zero");
        }
        return core::Status::ok();
    }
    if (application_worker_count == 0) {
        return core::Status::failure("game_application.invalid_worker_count",
                                     "application worker count must be non-zero");
    }
    if (window.width == 0 || window.height == 0) {
        return core::Status::failure("game_application.invalid_window_extent",
                                     "native application window extent must be non-zero");
    }
    if (shader_root.empty()) {
        return core::Status::failure("game_application.missing_shader_root",
                                     "native application requires a shader asset root");
    }
    if (voxel_palette == nullptr) {
        return core::Status::failure("game_application.missing_voxel_palette",
                                     "native application requires the resolved voxel palette");
    }
    return core::Status::ok();
}

float GameApplicationFrame::delta_seconds() const noexcept {
    return static_cast<float>(delta_microseconds) / 1'000'000.0F;
}

GameApplicationServices::GameApplicationServices(GameApplication& application) noexcept
    : application_(&application) {}

renderer::Renderer* GameApplicationServices::renderer() noexcept {
    return application_ != nullptr && application_->renderer_.is_initialized()
               ? &application_->renderer_
               : nullptr;
}

const renderer::Renderer* GameApplicationServices::renderer() const noexcept {
    return application_ != nullptr && application_->renderer_.is_initialized()
               ? &application_->renderer_
               : nullptr;
}

audio::IAudioSystem* GameApplicationServices::audio() noexcept {
    return application_ == nullptr ? nullptr : application_->audio_.get();
}

const audio::IAudioSystem* GameApplicationServices::audio() const noexcept {
    return application_ == nullptr ? nullptr : application_->audio_.get();
}

jobs::IJobSystem* GameApplicationServices::jobs() noexcept {
    return application_ == nullptr ? nullptr : application_->jobs_.get();
}

const jobs::IJobSystem* GameApplicationServices::jobs() const noexcept {
    return application_ == nullptr ? nullptr : application_->jobs_.get();
}

core::Status
GameApplicationServices::install_audio_system(std::unique_ptr<audio::IAudioSystem> audio_system) {
    if (application_ == nullptr) {
        return core::Status::failure("game_application.services_detached",
                                     "application services are no longer attached");
    }
    if (audio_system == nullptr) {
        return core::Status::failure("game_application.invalid_audio",
                                     "installed audio system must not be null");
    }
    if (application_->audio_ != nullptr) {
        return core::Status::failure("game_application.audio_already_installed",
                                     "application already owns an audio system");
    }
    application_->audio_ = std::move(audio_system);
    return core::Status::ok();
}

core::Status GameApplicationServices::set_cursor_capture(bool captured) {
    if (application_ == nullptr || application_->platform_ == nullptr ||
        !application_->window_.is_valid()) {
        return core::Status::failure("game_application.window_unavailable",
                                     "cursor capture requires a native application window");
    }
    return application_->platform_->set_cursor_capture(application_->window_, captured);
}

core::Status GameApplicationServices::set_clipboard_text(std::string text) {
    if (application_ == nullptr || application_->platform_ == nullptr) {
        return core::Status::failure("game_application.platform_unavailable",
                                     "application platform is not initialized");
    }
    return application_->platform_->set_clipboard_text(std::move(text));
}

void GameApplicationServices::request_quit() noexcept {
    if (application_ != nullptr && application_->platform_ != nullptr) {
        application_->platform_->request_quit();
    }
}

bool GameApplicationServices::headless() const noexcept {
    return application_ == nullptr || application_->config_.headless;
}

GameApplication::GameApplication(GameApplicationConfig config) : config_(std::move(config)) {}

GameApplication::~GameApplication() {
    (void)shutdown_shell();
}

core::Result<GameApplicationRunReport> GameApplication::run(IGameApplicationMode& mode) {
    if (running_) {
        return core::Result<GameApplicationRunReport>::failure("game_application.already_running",
                                                               "application is already running");
    }
    auto status = config_.validate();
    if (!status) {
        return core::Result<GameApplicationRunReport>::failure(status.error().code,
                                                               status.error().message);
    }
    status = initialize_shell();
    if (!status) {
        (void)shutdown_shell();
        return core::Result<GameApplicationRunReport>::failure(status.error().code,
                                                               status.error().message);
    }

    running_ = true;
    GameApplicationServices services(*this);
    std::optional<core::Error> first_error;
    bool mode_started = true;
    status = mode.initialize(services);
    if (!status) {
        first_error = status.error();
    }

    GameApplicationRunReport report;
    report.headless = config_.headless;
    std::uint64_t simulated_microseconds = 0;
    auto previous_time = std::chrono::steady_clock::now();

    while (!first_error.has_value() && platform_ != nullptr && !platform_->should_quit() &&
           (!config_.maximum_frames.has_value() || report.frame_count < *config_.maximum_frames)) {
        std::optional<platform::WindowInputSnapshot> input;
        std::uint64_t delta_microseconds = 16'667;
        std::int64_t now_milliseconds = 0;
        if (config_.headless) {
            simulated_microseconds += delta_microseconds;
            now_milliseconds = static_cast<std::int64_t>(simulated_microseconds / 1'000U);
        } else {
            status = pump_platform_events();
            if (!status) {
                first_error = status.error();
                break;
            }
            if (platform_->should_quit()) {
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(now - previous_time);
            previous_time = now;
            delta_microseconds = static_cast<std::uint64_t>(std::clamp<std::int64_t>(
                elapsed.count(), 1,
                static_cast<std::int64_t>(config_.maximum_frame_delta_microseconds)));
            now_milliseconds = platform_->clock().now_ms();
            input = platform_->input_snapshot(window_);
            if (!input.has_value()) {
                first_error = core::Error{"game_application.input_unavailable",
                                          "platform did not provide an input snapshot"};
                break;
            }
        }

        const GameApplicationFrame frame{report.frame_count,
                                         delta_microseconds,
                                         now_milliseconds,
                                         platform::PlatformClock::wall_time_ms(),
                                         extent_,
                                         input.has_value() ? &*input : nullptr,
                                         config_.headless};
        auto output = mode.update(services, frame);
        if (!output) {
            first_error = output.error();
            break;
        }
        if (output.value().render.has_value() && !minimized_) {
            if (config_.headless || !renderer_.is_initialized()) {
                first_error =
                    core::Error{"game_application.unavailable_renderer",
                                "application mode requested rendering without a native renderer"};
                break;
            }
            auto rendered = renderer_.render_frame(*output.value().render);
            if (!rendered) {
                first_error = rendered.error();
                break;
            }
        } else if (!config_.headless) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++report.frame_count;
    }

    report.mode_summary = mode.summary();
    if (mode_started) {
        status = mode.shutdown(services);
        if (!status && !first_error.has_value()) {
            first_error = status.error();
        }
    }
    status = shutdown_shell();
    if (!status && !first_error.has_value()) {
        first_error = status.error();
    }
    running_ = false;

    if (first_error.has_value()) {
        return core::Result<GameApplicationRunReport>::failure(first_error->code,
                                                               first_error->message);
    }
    return core::Result<GameApplicationRunReport>::success(std::move(report));
}

core::Status GameApplication::initialize_shell() {
    auto created_jobs = jobs::create_job_system(
        {config_.headless ? jobs::JobBackend::immediate : jobs::JobBackend::thread_pool,
         config_.application_worker_count});
    if (!created_jobs) {
        return core::Status::failure(created_jobs.error().code, created_jobs.error().message);
    }
    jobs_ = std::move(created_jobs).value();
    auto created_platform =
        platform::create_platform({config_.headless ? platform::PlatformBackend::headless
                                                    : platform::PlatformBackend::native});
    if (!created_platform) {
        return core::Status::failure(created_platform.error().code,
                                     created_platform.error().message);
    }
    platform_ = std::move(created_platform).value();
    if (config_.headless) {
        extent_ = {};
        return core::Status::ok();
    }

    auto created_window = platform_->create_window(config_.window);
    if (!created_window) {
        return core::Status::failure(created_window.error().code, created_window.error().message);
    }
    window_ = created_window.value();
    extent_ = {config_.window.width, config_.window.height};

    auto native_handle = platform_->native_window_handle(window_);
    if (!native_handle.has_value()) {
        return core::Status::failure(
            "game_application.native_handle_unavailable",
            "native platform did not expose a Vulkan-compatible window handle");
    }
    renderer::rhi::RenderDeviceDesc device_desc;
    device_desc.backend = renderer::rhi::RenderBackend::vulkan;
    device_desc.application_name = config_.window.title;
    device_desc.initial_extent = extent_;
    device_desc.present_mode = config_.present_mode;
    device_desc.enable_validation = config_.enable_render_validation;
    device_desc.native_window = *native_handle;
    auto device = renderer::rhi::create_render_device(device_desc);
    if (!device) {
        return core::Status::failure(device.error().code, device.error().message);
    }
    auto shaders = load_application_shaders(config_.shader_root);
    if (!shaders) {
        return core::Status::failure(shaders.error().code, shaders.error().message);
    }

    renderer::RendererInitDesc renderer_desc;
    auto ui_font =
        core::read_binary_file(config_.shader_root.parent_path() / "fonts/heartstead-ui.ttf");
    if (!ui_font) {
        return core::Status::failure(ui_font.error().code, ui_font.error().message);
    }
    renderer_desc.device = std::move(device).value();
    renderer_desc.sky_vertex_spirv = std::move(shaders.value().sky_vertex);
    renderer_desc.sky_fragment_spirv = std::move(shaders.value().sky_fragment);
    renderer_desc.terrain_vertex_spirv = std::move(shaders.value().terrain_vertex);
    renderer_desc.far_terrain_vertex_spirv = std::move(shaders.value().far_terrain_vertex);
    renderer_desc.terrain_fragment_spirv = std::move(shaders.value().terrain_fragment);
    renderer_desc.static_mesh_vertex_spirv = std::move(shaders.value().static_vertex);
    renderer_desc.static_mesh_fragment_spirv = std::move(shaders.value().static_fragment);
    renderer_desc.shadow_terrain_fragment_spirv =
        std::move(shaders.value().shadow_terrain_fragment);
    renderer_desc.shadow_static_fragment_spirv = std::move(shaders.value().shadow_static_fragment);
    renderer_desc.debug_vertex_spirv = std::move(shaders.value().debug_vertex);
    renderer_desc.debug_fragment_spirv = std::move(shaders.value().debug_fragment);
    renderer_desc.tone_map_vertex_spirv = std::move(shaders.value().tone_map_vertex);
    renderer_desc.tone_map_fragment_spirv = std::move(shaders.value().tone_map_fragment);
    renderer_desc.ssao_fragment_spirv = std::move(shaders.value().ssao_fragment);
    renderer_desc.ao_composite_fragment_spirv = std::move(shaders.value().ao_composite_fragment);
    renderer_desc.fxaa_fragment_spirv = std::move(shaders.value().fxaa_fragment);
    renderer_desc.bloom_fragment_spirv = std::move(shaders.value().bloom_fragment);
    renderer_desc.ui_vertex_spirv = std::move(shaders.value().ui_vertex);
    renderer_desc.ui_fragment_spirv = std::move(shaders.value().ui_fragment);
    renderer_desc.ui_font_bytes = std::move(ui_font).value();
    renderer_desc.quality_preset = config_.renderer_quality;
    renderer_desc.voxel_palette = config_.voxel_palette;
    renderer_desc.terrain_material_assets = std::move(config_.terrain_material_assets);
    return renderer_.initialize(std::move(renderer_desc));
}

core::Status GameApplication::pump_platform_events() {
    platform_->begin_frame();
    while (auto event = platform_->poll_event()) {
        if (event->kind == platform::PlatformEventKind::quit_requested ||
            event->kind == platform::PlatformEventKind::window_closed) {
            platform_->request_quit();
            continue;
        }
        if (event->kind == platform::PlatformEventKind::window_resized &&
            event->window_id == window_) {
            const renderer::rhi::RenderExtent requested_extent{event->width, event->height};
            if (!requested_extent.is_valid()) {
                minimized_ = true;
                continue;
            }
            auto status = renderer_.resize(requested_extent);
            if (!status) {
                return status;
            }
            extent_ = requested_extent;
            minimized_ = false;
        }
    }
    return core::Status::ok();
}

core::Status GameApplication::shutdown_shell() {
    core::Status first_failure = core::Status::ok();
    const auto remember_failure = [&first_failure](core::Status status) {
        if (!status && first_failure) {
            first_failure = std::move(status);
        }
    };

    audio_.reset();
    jobs_.reset();
    if (renderer_.is_initialized()) {
        remember_failure(renderer_.shutdown());
    }
    if (platform_ != nullptr && window_.is_valid()) {
        const auto* state = platform_->find_window(window_);
        if (state != nullptr && state->open) {
            remember_failure(platform_->close_window(window_));
        }
    }
    window_ = {};
    extent_ = {};
    platform_.reset();
    return first_failure;
}

} // namespace heartstead::game
