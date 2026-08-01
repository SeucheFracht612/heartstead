#include "engine/assets/cooked_asset_store.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/renderer/materials/terrain_material_assets.hpp"
#include "game/application/application_settings.hpp"
#include "game/application/game_application.hpp"
#include "game/application/heartstead_application_mode.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace heartstead;

struct LaunchOptions {
    bool headless = false;
    std::optional<std::uint64_t> maximum_frames;
    bool help = false;
};

[[nodiscard]] core::Result<std::uint64_t> parse_frame_count(std::string_view value) {
    std::uint64_t frames = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), frames);
    if (error != std::errc{} || end != value.data() + value.size() || frames == 0) {
        return core::Result<std::uint64_t>::failure(
            "heartstead.invalid_frame_count", "frame count must be a positive 64-bit integer");
    }
    return core::Result<std::uint64_t>::success(frames);
}

[[nodiscard]] core::Result<LaunchOptions> parse_options(int argc, char** argv) {
    LaunchOptions options;
    for (int index = 1; index < argc; ++index) {
        const auto argument = std::string_view(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--frames" || argument == "--native-frames") {
            if (index + 1 >= argc) {
                return core::Result<LaunchOptions>::failure("heartstead.missing_frame_count",
                                                            std::string(argument) + " requires N");
            }
            auto frames = parse_frame_count(argv[++index]);
            if (!frames) {
                return core::Result<LaunchOptions>::failure(frames.error().code,
                                                            frames.error().message);
            }
            options.maximum_frames = frames.value();
            if (argument == "--frames") {
                options.headless = true;
            }
        } else {
            return core::Result<LaunchOptions>::failure("heartstead.unknown_option",
                                                        "unknown option: " + std::string(argument));
        }
    }
    return core::Result<LaunchOptions>::success(std::move(options));
}

void print_usage(const char* executable, std::ostream& output) {
    output << "usage: " << executable
           << " [--headless] [--frames N] [--native-frames N]\n"
              "       --frames implies --headless for deterministic smoke runs\n";
}

int fail(const core::Error& error) {
    std::cerr << error.code << ": " << error.message << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    return heartstead::core::run_process_entry(argv[0], [argc, argv] {
        auto parsed = parse_options(argc, argv);
        if (!parsed) {
            print_usage(argv[0], std::cerr);
            std::cerr << parsed.error().code << ": " << parsed.error().message << '\n';
            return 2;
        }
        const auto options = std::move(parsed).value();
        if (options.help) {
            print_usage(argv[0], std::cout);
            return 0;
        }

        const auto source_root = std::filesystem::path{HEARTSTEAD_SOURCE_ROOT};
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
        const auto content_report = heartstead::content::ContentValidation::validate(source_root);
        if (content_report.has_errors()) {
            return fail({"heartstead.content_validation_failed",
                         "content validation reported one or more errors"});
        }

        std::optional<heartstead::assets::CookedAssetStore> cooked_assets;
        heartstead::renderer::materials::TerrainMaterialAssetSet terrain_assets;
        if (!options.headless) {
            auto loaded = heartstead::assets::CookedAssetStore::load(
                std::filesystem::path{HEARTSTEAD_GAME_COOKED_ASSET_DIR});
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
        application_config.shader_root =
            std::filesystem::path{HEARTSTEAD_GAME_ASSET_DIR} / "shaders";
        application_config.voxel_palette = &content_report.voxel_palette;
        application_config.terrain_material_assets = std::move(terrain_assets);
        application_config.renderer_quality = application_settings.rendering_quality;

        heartstead::game::HeartsteadApplicationModeConfig mode_config;
        mode_config.content_report = &content_report;
        mode_config.cooked_assets = cooked_assets.has_value() ? &*cooked_assets : nullptr;
        mode_config.cooked_asset_root = HEARTSTEAD_GAME_COOKED_ASSET_DIR;
        mode_config.user_data_root = user_data_root;
        mode_config.initial_settings = application_settings;
        mode_config.headless = options.headless;

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
