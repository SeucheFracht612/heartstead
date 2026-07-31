#include "apps/asset_lab/asset_lab_mode.hpp"

#include "engine/assets/cooked_asset_store.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/process_entry.hpp"
#include "engine/renderer/materials/terrain_material_assets.hpp"
#include "game/application/game_application.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace heartstead;

struct Options {
    asset_lab::PreviewKind preview = asset_lab::PreviewKind::visual_prefab;
    asset_lab::LightingPreset lighting = asset_lab::LightingPreset::studio;
    renderer::LightingDebugView debug = renderer::LightingDebugView::none;
    std::string selection;
    std::vector<entities::VisualStateValue> states;
    std::optional<std::uint32_t> lod;
    std::string equipment;
    std::string socket;
    std::uint64_t frames = 1U;
    bool headless = false;
    bool show_bounds = false;
    bool show_skeleton = false;
    bool help = false;
    bool list = false;
};

[[nodiscard]] core::Result<std::uint64_t> parse_frames(std::string_view value) {
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed == 0U) {
        return core::Result<std::uint64_t>::failure(
            "asset_lab.invalid_frames", "Asset Lab frame count must be a positive integer");
    }
    return core::Result<std::uint64_t>::success(parsed);
}

[[nodiscard]] core::Result<entities::VisualStateValue> parse_state(std::string_view value) {
    const auto separator = value.find('=');
    if (separator == std::string_view::npos || separator == 0U || separator + 1U == value.size()) {
        return core::Result<entities::VisualStateValue>::failure(
            "asset_lab.invalid_state", "visual state must use CHANNEL=VALUE");
    }
    return core::Result<entities::VisualStateValue>::success(
        {std::string(value.substr(0, separator)), std::string(value.substr(separator + 1U))});
}

[[nodiscard]] core::Result<Options> parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const auto next = [&]() -> core::Result<std::string_view> {
            if (index + 1 >= argc) {
                return core::Result<std::string_view>::failure(
                    "asset_lab.missing_argument", std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(argv[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--list") {
            options.list = true;
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--bounds") {
            options.show_bounds = true;
        } else if (argument == "--skeleton") {
            options.show_skeleton = true;
        } else if (argument == "--preview") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = asset_lab::parse_preview_kind(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("asset_lab.invalid_preview",
                                                      "unknown Asset Lab preview: " +
                                                          std::string(value.value()));
            }
            options.preview = *parsed;
        } else if (argument == "--lighting") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = asset_lab::parse_lighting_preset(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("asset_lab.invalid_lighting",
                                                      "unknown Asset Lab lighting preset: " +
                                                          std::string(value.value()));
            }
            options.lighting = *parsed;
        } else if (argument == "--debug") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            const auto parsed = asset_lab::parse_asset_lab_debug_view(value.value());
            if (!parsed) {
                return core::Result<Options>::failure("asset_lab.invalid_debug_view",
                                                      "unknown Asset Lab debug view: " +
                                                          std::string(value.value()));
            }
            options.debug = *parsed;
        } else if (argument == "--asset" || argument == "--prefab") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            options.selection = value.value();
        } else if (argument == "--state") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            auto parsed = parse_state(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(parsed.error().code, parsed.error().message);
            }
            options.states.push_back(std::move(parsed).value());
        } else if (argument == "--equipment" || argument == "--socket") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            (argument == "--equipment" ? options.equipment : options.socket) = value.value();
        } else if (argument == "--lod") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            std::uint32_t lod = 0;
            const auto [end, error] = std::from_chars(
                value.value().data(), value.value().data() + value.value().size(), lod);
            if (error != std::errc{} || end != value.value().data() + value.value().size()) {
                return core::Result<Options>::failure("asset_lab.invalid_lod",
                                                      "--lod requires an unsigned integer");
            }
            options.lod = lod;
        } else if (argument == "--frames" || argument == "--native-frames") {
            auto value = next();
            if (!value) {
                return core::Result<Options>::failure(value.error().code, value.error().message);
            }
            auto parsed = parse_frames(value.value());
            if (!parsed) {
                return core::Result<Options>::failure(parsed.error().code, parsed.error().message);
            }
            options.frames = parsed.value();
            if (argument == "--frames") {
                options.headless = true;
            }
        } else {
            return core::Result<Options>::failure(
                "asset_lab.unknown_option", "unknown Asset Lab option: " + std::string(argument));
        }
    }
    if (options.equipment.empty() != options.socket.empty()) {
        return core::Result<Options>::failure("asset_lab.incomplete_equipment",
                                              "--equipment and --socket must be supplied together");
    }
    return core::Result<Options>::success(std::move(options));
}

void print_usage(std::ostream& output) {
    output << "usage: heartstead_asset_lab [options]\n"
              "  --asset ID | --prefab ID     Select a cooked asset or visual prefab\n"
              "  --preview MODE               static-model, animated-model, character, equipment,\n"
              "                               terrain-material, vegetation, particle, material,\n"
              "                               texture, lod, visual-prefab\n"
              "  --lighting PRESET            studio, overcast, noon, sunset, night,\n"
              "                               fire-lit-interior, cave, forest-shade,\n"
              "                               rain-wetness, snow-frost, underwater\n"
              "  --debug VIEW                 material channels, UVs, tangents, vertex colors,\n"
              "                               mip-level, texel-density, texture-residency, LOD,\n"
              "                               bounds, skeletons, skin-weights, cascades, overdraw\n"
              "  --state CHANNEL=VALUE        Apply repeatable gameplay-owned prefab state\n"
              "  --lod N                      Inspect a specific prefab LOD\n"
              "  --equipment MODEL --socket NAME\n"
              "  --bounds | --skeleton        Draw model diagnostics\n"
              "  --headless                    Validate and inspect without a display\n"
              "  --frames N                    Bounded headless inspection\n"
              "  --native-frames N             Bounded Vulkan preview\n"
              "  --list                         List all preview, lighting, and debug names\n";
}

void print_lists() {
    constexpr std::array previews{
        asset_lab::PreviewKind::static_model,     asset_lab::PreviewKind::animated_model,
        asset_lab::PreviewKind::character,        asset_lab::PreviewKind::equipment,
        asset_lab::PreviewKind::terrain_material, asset_lab::PreviewKind::vegetation,
        asset_lab::PreviewKind::particle,         asset_lab::PreviewKind::material,
        asset_lab::PreviewKind::texture,          asset_lab::PreviewKind::lod,
        asset_lab::PreviewKind::visual_prefab,
    };
    constexpr std::array lights{
        asset_lab::LightingPreset::studio,       asset_lab::LightingPreset::overcast,
        asset_lab::LightingPreset::noon,         asset_lab::LightingPreset::sunset,
        asset_lab::LightingPreset::night,        asset_lab::LightingPreset::fire_lit_interior,
        asset_lab::LightingPreset::cave,         asset_lab::LightingPreset::forest_shade,
        asset_lab::LightingPreset::rain_wetness, asset_lab::LightingPreset::snow_frost,
        asset_lab::LightingPreset::underwater,
    };
    std::cout << "previews:";
    for (const auto value : previews) {
        std::cout << ' ' << asset_lab::preview_kind_name(value);
    }
    std::cout << "\nlighting:";
    for (const auto value : lights) {
        std::cout << ' ' << asset_lab::lighting_preset_name(value);
    }
    std::cout << "\ndebug:";
    for (std::uint32_t value = 0;
         value <= static_cast<std::uint32_t>(renderer::LightingDebugView::overdraw); ++value) {
        std::cout << ' '
                  << asset_lab::asset_lab_debug_view_name(
                         static_cast<renderer::LightingDebugView>(value));
    }
    std::cout << '\n';
}

[[nodiscard]] int fail(const core::Error& error) {
    std::cerr << error.code << ": " << error.message << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    return heartstead::core::run_process_entry(argv[0], [argc, argv] {
        auto options = parse_options(argc, argv);
        if (!options) {
            print_usage(std::cerr);
            return fail(options.error());
        }
        if (options.value().help) {
            print_usage(std::cout);
            return 0;
        }
        if (options.value().list) {
            print_lists();
            return 0;
        }
        const auto content_report = heartstead::content::ContentValidation::validate(
            std::filesystem::path{HEARTSTEAD_SOURCE_ROOT});
        if (content_report.has_errors()) {
            std::cerr << "asset_lab.content_invalid: project content validation failed\n";
            return 1;
        }
        auto cooked_assets = heartstead::assets::CookedAssetStore::load(
            std::filesystem::path{HEARTSTEAD_ASSET_LAB_COOKED_ASSET_DIR});
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

        heartstead::asset_lab::AssetLabModeConfig mode_config;
        mode_config.content = &content_report;
        mode_config.cooked_asset_root =
            std::filesystem::path{HEARTSTEAD_ASSET_LAB_COOKED_ASSET_DIR};
        mode_config.preview = options.value().preview;
        mode_config.lighting = options.value().lighting;
        mode_config.debug_view = options.value().debug;
        mode_config.selection_id = options.value().selection;
        mode_config.visual_states = options.value().states;
        mode_config.forced_lod = options.value().lod;
        mode_config.equipment_asset = options.value().equipment;
        mode_config.equipment_socket = options.value().socket;
        mode_config.show_bounds = options.value().show_bounds;
        mode_config.show_skeleton = options.value().show_skeleton;

        heartstead::game::GameApplicationConfig application_config;
        application_config.headless = options.value().headless;
        application_config.maximum_frames = options.value().frames;
        application_config.window = {"Heartstead Asset Lab", 1280, 720, true};
        application_config.shader_root =
            std::filesystem::path{HEARTSTEAD_ASSET_LAB_ASSET_DIR} / "shaders";
        application_config.voxel_palette = &content_report.voxel_palette;
        application_config.terrain_material_assets = std::move(terrain_material_assets).value();

        heartstead::game::GameApplication application(std::move(application_config));
        heartstead::asset_lab::AssetLabMode mode(std::move(mode_config));
        auto report = application.run(mode);
        if (!report) {
            return fail(report.error());
        }
        std::cout << report.value().mode_summary << '\n';
        return 0;
    });
}
