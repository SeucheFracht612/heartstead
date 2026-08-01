#include "engine/assets/cooked_asset_store.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/renderer/materials/terrain_material_assets.hpp"
#include "game/application/application_settings.hpp"
#include "game/application/game_application.hpp"
#include "game/application/heartstead_application_mode.hpp"
#include "game/application/launch_options.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

namespace {

using namespace heartstead;

[[nodiscard]] core::Result<game::HeartsteadLaunchOptions> parse_options(int argc, char** argv) {
    std::vector<std::string_view> arguments;
    arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return game::parse_heartstead_launch_options(arguments);
}

int fail(const core::Error& error) {
    std::cerr << error.code << ": " << error.message << '\n';
    return 1;
}

[[nodiscard]] std::filesystem::path executable_directory(const char* argument_zero) {
#if defined(__linux__)
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error && !executable.empty()) {
        return executable.parent_path();
    }
#endif
    std::error_code fallback_error;
    auto fallback_executable = std::filesystem::absolute(
        argument_zero != nullptr ? std::filesystem::path{argument_zero} : std::filesystem::path{},
        fallback_error);
    if (fallback_error || fallback_executable.empty()) {
        return std::filesystem::current_path();
    }
    return fallback_executable.parent_path();
}

} // namespace

int main(int argc, char** argv) {
    return heartstead::core::run_process_entry(argv[0], [argc, argv] {
        auto parsed = parse_options(argc, argv);
        if (!parsed) {
            std::cerr << heartstead::game::heartstead_command_line_usage(argv[0]);
            std::cerr << parsed.error().code << ": " << parsed.error().message << '\n';
            return 2;
        }
        const auto options = std::move(parsed).value();
        if (options.show_help) {
            std::cout << heartstead::game::heartstead_command_line_usage(argv[0]);
            return 0;
        }
        if (options.show_version) {
            std::cout << "Heartstead 0.1.0\n";
            return 0;
        }

        const auto application_root = executable_directory(argv[0]);
        const auto content_root = application_root / HEARTSTEAD_GAME_CONTENT_DIR;
        const auto user_data_root = heartstead::game::default_application_data_root();
        heartstead::game::ApplicationSettings application_settings;
        const heartstead::game::ApplicationSettingsStore settings_store(user_data_root /
                                                                        "settings.txt");
        auto loaded_settings = settings_store.load();
        if (loaded_settings) {
            application_settings = std::move(loaded_settings).value();
        } else {
            std::cerr << loaded_settings.error().code << ": " << loaded_settings.error().message
                      << " (using defaults)\n";
        }
        const auto content_report = heartstead::content::ContentValidation::validate(content_root);
        if (content_report.has_errors()) {
            return fail({"heartstead.content_validation_failed",
                         "content validation reported one or more errors"});
        }

        std::optional<heartstead::assets::CookedAssetStore> cooked_assets;
        heartstead::renderer::materials::TerrainMaterialAssetSet terrain_assets;
        if (!options.headless) {
            auto loaded = heartstead::assets::CookedAssetStore::load(
                application_root / HEARTSTEAD_GAME_COOKED_ASSET_DIR);
            if (!loaded) {
                return fail(loaded.error());
            }
            cooked_assets.emplace(std::move(loaded).value());
            auto terrain = heartstead::renderer::materials::load_terrain_material_assets(
                content_report.voxel_palette, content_report.material_registry, *cooked_assets);
            if (!terrain) {
                return fail(terrain.error());
            }
            terrain_assets = std::move(terrain).value();
        }

        heartstead::game::GameApplicationConfig application_config;
        application_config.headless = options.headless;
        application_config.maximum_frames = options.maximum_frames;
        application_config.window = {"Heartstead", application_settings.window_width,
                                     application_settings.window_height, true};
        application_config.shader_root = application_root / HEARTSTEAD_GAME_ASSET_DIR / "shaders";
        application_config.voxel_palette = &content_report.voxel_palette;
        application_config.terrain_material_assets = std::move(terrain_assets);
        application_config.renderer_quality =
            options.safe_mode ? heartstead::renderer::RendererQualityPreset::low
                              : application_settings.rendering_quality;

        heartstead::game::HeartsteadApplicationModeConfig mode_config;
        mode_config.content_report = &content_report;
        mode_config.cooked_assets = cooked_assets.has_value() ? &*cooked_assets : nullptr;
        mode_config.cooked_asset_root = HEARTSTEAD_GAME_COOKED_ASSET_DIR;
        mode_config.user_data_root = user_data_root;
        mode_config.initial_settings = application_settings;
        mode_config.initial_launch = options.initial_launch;
        mode_config.headless = options.headless;
        mode_config.safe_mode = options.safe_mode;

        heartstead::game::GameApplication application(std::move(application_config));
        heartstead::game::HeartsteadApplicationMode mode(std::move(mode_config));
        auto report = application.run(mode);
        if (!report) {
            return fail(report.error());
        }
        if (options.headless) {
            std::cout << report.value().mode_summary << '\n';
        }
        return 0;
    });
}
