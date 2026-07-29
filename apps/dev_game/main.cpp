#include "apps/dev_game/dev_game_mode.hpp"

#include "engine/content/content_validation.hpp"
#include "engine/core/process_entry.hpp"
#include "game/application/game_application.hpp"

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
    std::optional<net::TransportEndpoint> connect_endpoint;
    bool help = false;
};

[[nodiscard]] core::Result<std::uint64_t>
parse_positive_frame_count(std::string_view value, std::string_view option) {
    std::uint64_t frames = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), frames);
    if (error != std::errc{} || end != value.data() + value.size() || frames == 0) {
        return core::Result<std::uint64_t>::failure(
            "dev_game.invalid_frame_count",
            std::string(option) + " requires a positive 64-bit integer");
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
                return core::Result<LaunchOptions>::failure(
                    "dev_game.missing_frame_count",
                    std::string(argument) + " requires a positive integer");
            }
            auto frames = parse_positive_frame_count(argv[++index], argument);
            if (!frames) {
                return core::Result<LaunchOptions>::failure(frames.error().code,
                                                            frames.error().message);
            }
            options.maximum_frames = frames.value();
            if (argument == "--frames") {
                // Existing CI smoke runs rely on --frames never requiring a display.
                options.headless = true;
            }
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
           << " [--headless] [--frames N] [--native-frames N]"
              " [--connect ADDRESS:PORT]\n"
           << "       --frames implies --headless for deterministic smoke runs\n"
           << "       --native-frames bounds a real windowed smoke run\n";
}

int fail(const core::Error& error) {
    std::cerr << error.code << ": " << error.message << '\n';
    return 1;
}

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
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

        heartstead::game::GameApplicationConfig application_config;
        application_config.headless = options.headless;
        application_config.maximum_frames =
            options.maximum_frames.has_value()
                ? options.maximum_frames
                : std::optional<std::uint64_t>{options.headless ? 120U : 0U};
        if (!options.headless && !options.maximum_frames.has_value()) {
            application_config.maximum_frames.reset();
        }
        application_config.window = {"Heartstead Development Game", 1280, 720, true};
        application_config.shader_root =
            std::filesystem::path{HEARTSTEAD_DEV_GAME_ASSET_DIR} / "shaders";
        application_config.voxel_palette = &content_report.voxel_palette;

        heartstead::dev_game::DevGameModeConfig mode_config;
        mode_config.content_report = &content_report;
        mode_config.cooked_asset_root =
            std::filesystem::path{HEARTSTEAD_DEV_GAME_COOKED_ASSET_DIR};
        mode_config.connect_endpoint = options.connect_endpoint;
        mode_config.headless = options.headless;

        heartstead::game::GameApplication application(std::move(application_config));
        heartstead::dev_game::DevGameMode mode(std::move(mode_config));
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
