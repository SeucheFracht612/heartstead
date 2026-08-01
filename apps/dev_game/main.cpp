#include "engine/assets/cooked_asset_store.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/renderer/materials/terrain_material_assets.hpp"
#include "engine/save/save_database.hpp"
#include "game/application/game_application.hpp"
#include "game/application/heartstead_application_mode.hpp"
#include "game/foundation/foundation_world.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace {

using namespace heartstead;

constexpr std::string_view development_scenario_id = "base:scenarios/foundation_slice";

struct LaunchOptions {
    bool headless = false;
    std::optional<std::uint64_t> maximum_frames;
    std::optional<net::TransportEndpoint> connect_endpoint;
    std::optional<std::filesystem::path> save_root;
    std::int64_t autosave_interval_ms = 30'000;
    bool disable_persistence = false;
    bool diagnostic_asset_fallbacks = false;
    bool help = false;
};

[[nodiscard]] core::Result<std::uint64_t> parse_positive_frame_count(std::string_view value,
                                                                     std::string_view option) {
    std::uint64_t frames = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), frames);
    if (error != std::errc{} || end != value.data() + value.size() || frames == 0) {
        return core::Result<std::uint64_t>::failure("dev_game.invalid_frame_count",
                                                    std::string(option) +
                                                        " requires a positive 64-bit integer");
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
                return core::Result<LaunchOptions>::failure("dev_game.missing_frame_count",
                                                            std::string(argument) +
                                                                " requires a positive integer");
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
                return core::Result<LaunchOptions>::failure("dev_game.missing_connect_endpoint",
                                                            "--connect requires ADDRESS:PORT");
            }
            auto endpoint = net::parse_transport_endpoint(argv[++index], 7777);
            if (!endpoint) {
                return core::Result<LaunchOptions>::failure(endpoint.error().code,
                                                            endpoint.error().message);
            }
            options.connect_endpoint = std::move(endpoint).value();
        } else if (argument == "--save") {
            if (index + 1 >= argc || std::string_view(argv[index + 1]).empty()) {
                return core::Result<LaunchOptions>::failure("dev_game.missing_save_path",
                                                            "--save requires a directory path");
            }
            options.save_root = std::filesystem::path(argv[++index]);
        } else if (argument == "--autosave-seconds") {
            if (index + 1 >= argc) {
                return core::Result<LaunchOptions>::failure(
                    "dev_game.missing_autosave_interval",
                    "--autosave-seconds requires a positive integer");
            }
            auto seconds = parse_positive_frame_count(argv[++index], argument);
            if (!seconds ||
                seconds.value() >
                    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1'000)) {
                return core::Result<LaunchOptions>::failure(
                    "dev_game.invalid_autosave_interval",
                    "--autosave-seconds exceeds the supported interval");
            }
            options.autosave_interval_ms = static_cast<std::int64_t>(seconds.value() * 1'000);
        } else if (argument == "--no-save") {
            options.disable_persistence = true;
        } else if (argument == "--diagnostic-asset-fallbacks") {
            options.diagnostic_asset_fallbacks = true;
        } else {
            return core::Result<LaunchOptions>::failure("dev_game.unknown_option",
                                                        "unknown option: " + std::string(argument));
        }
    }
    if (options.disable_persistence && options.save_root.has_value()) {
        return core::Result<LaunchOptions>::failure("dev_game.conflicting_save_options",
                                                    "--save and --no-save cannot be used together");
    }
    if (options.connect_endpoint.has_value() && options.save_root.has_value()) {
        return core::Result<LaunchOptions>::failure(
            "dev_game.remote_save_unsupported",
            "a remote client cannot own the authoritative world save");
    }
    return core::Result<LaunchOptions>::success(std::move(options));
}

void print_usage(const char* executable, std::ostream& output) {
    output << "usage: " << executable
           << " [--headless] [--frames N] [--native-frames N]"
              " [--connect ADDRESS:PORT] [--save DIRECTORY]"
              " [--autosave-seconds N] [--no-save] [--diagnostic-asset-fallbacks]\n"
           << "       --frames implies --headless for deterministic smoke runs\n"
           << "       --native-frames bounds a real windowed smoke run\n"
           << "       normal local launches persist to saves/foundation_slice_0_1\n"
           << "       bounded/headless runs persist only when --save is supplied\n"
           << "       --diagnostic-asset-fallbacks keeps the production diagnostic overlay enabled\n";
}

int fail(const core::Error& error) {
    std::cerr << error.code << ": " << error.message << '\n';
    return 1;
}

int fail(std::string_view message) {
    std::cerr << message << '\n';
    return 1;
}

[[nodiscard]] std::string endpoint_name(const net::TransportEndpoint& endpoint) {
    const auto address = endpoint.address.contains(':') ? "[" + endpoint.address + "]"
                                                        : endpoint.address;
    return address + ":" + std::to_string(endpoint.port);
}

[[nodiscard]] core::Result<game::SessionLaunchRequest>
prepare_session(const LaunchOptions& options,
                const content::ContentValidationReport& content_report) {
    auto metadata = content::save_metadata_from_content_report(
        content_report, "Foundation Slice 0.1", game::foundation::world_seed);
    if (!metadata) {
        return core::Result<game::SessionLaunchRequest>::failure(metadata.error().code,
                                                                 metadata.error().message);
    }

    game::SessionLaunchRequest request;
    request.world_name = "Foundation Development Slice";
    request.scenario_id = std::string(development_scenario_id);
    request.seed = game::foundation::world_seed;
    request.generator_preset = "temperate_valley";
    request.metadata = std::move(metadata).value();
    request.runtime.headless = options.headless;
    request.runtime.physics_backend = options.headless ? physics::PhysicsBackend::headless
                                                       : physics::PhysicsBackend::jolt;
    if (options.diagnostic_asset_fallbacks) {
        request.initial_runtime_options.emplace_back("diagnostics");
    }

    if (options.connect_endpoint.has_value()) {
        request.mode = game::SessionMode::remote_multiplayer;
        request.world_source = game::WorldSourceKind::remote_server;
        request.persistence = game::PersistencePolicy::ephemeral;
        request.network_endpoint = options.connect_endpoint;
        request.world_name = endpoint_name(*options.connect_endpoint);
        return core::Result<game::SessionLaunchRequest>::success(std::move(request));
    }

    request.mode = game::SessionMode::local_single_player;
    request.world_source = game::WorldSourceKind::developer_scenario;
    request.persistence = game::PersistencePolicy::ephemeral;

    std::optional<std::filesystem::path> save_root;
    if (!options.disable_persistence) {
        if (options.save_root.has_value()) {
            save_root = options.save_root;
        } else if (!options.headless && !options.maximum_frames.has_value()) {
            save_root = std::filesystem::path{HEARTSTEAD_SOURCE_ROOT} /
                        "saves/foundation_slice_0_1";
        }
    }
    if (!save_root.has_value()) {
        return core::Result<game::SessionLaunchRequest>::success(std::move(request));
    }

    save::FileSaveDatabase database(*save_root);
    auto stats = database.stats();
    if (!stats) {
        return core::Result<game::SessionLaunchRequest>::failure(stats.error().code,
                                                                 stats.error().message);
    }
    request.persistence = game::PersistencePolicy::persistent;
    request.save_path = *save_root;
    if (stats.value().has_snapshot) {
        request.world_source = game::WorldSourceKind::existing_save;
        request.scenario_id.clear();
        request.seed.reset();
        request.generator_preset.clear();
    }
    return core::Result<game::SessionLaunchRequest>::success(std::move(request));
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
        auto cooked_assets = heartstead::assets::CookedAssetStore::load(
            std::filesystem::path{HEARTSTEAD_DEV_GAME_COOKED_ASSET_DIR});
        if (!cooked_assets) {
            return fail(cooked_assets.error());
        }
        auto terrain_material_assets =
            heartstead::renderer::materials::load_terrain_material_assets(
                content_report.voxel_palette, content_report.material_registry,
                cooked_assets.value());
        if (!terrain_material_assets) {
            return fail(terrain_material_assets.error());
        }
        auto initial_session = prepare_session(options, content_report);
        if (!initial_session) {
            return fail(initial_session.error());
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
        application_config.terrain_material_assets =
            std::move(terrain_material_assets).value();

        heartstead::game::HeartsteadApplicationModeConfig mode_config;
        mode_config.content_report = &content_report;
        mode_config.cooked_assets = &cooked_assets.value();
        mode_config.cooked_asset_root =
            std::filesystem::path{HEARTSTEAD_DEV_GAME_COOKED_ASSET_DIR};
        mode_config.user_data_root = std::filesystem::path{HEARTSTEAD_DEV_GAME_DATA_DIR};
        mode_config.initial_session = std::move(initial_session).value();
        mode_config.autosave_interval_ms = options.autosave_interval_ms;
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
