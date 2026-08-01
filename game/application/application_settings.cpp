#include "game/application/application_settings.hpp"

#include "engine/core/filesystem.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace heartstead::game {

namespace {

constexpr std::string_view settings_magic = "heartstead.application_settings.v1";
constexpr std::uintmax_t maximum_settings_bytes = 64U * 1024U;

[[nodiscard]] core::Status failure(std::string code, const std::error_code& error) {
    return core::Status::failure(std::move(code), error.message());
}

[[nodiscard]] bool safe_text(std::string_view value, std::size_t maximum) noexcept {
    return value.size() <= maximum && std::ranges::none_of(value, [](char character) {
               const auto byte = static_cast<unsigned char>(character);
               return byte < 0x20 || byte > 0x7e || character == '|';
           });
}

[[nodiscard]] std::string quality_name(renderer::RendererQualityPreset quality) {
    switch (quality) {
    case renderer::RendererQualityPreset::low:
        return "low";
    case renderer::RendererQualityPreset::medium:
        return "medium";
    case renderer::RendererQualityPreset::high:
        return "high";
    case renderer::RendererQualityPreset::ultra:
        return "ultra";
    }
    return "high";
}

[[nodiscard]] core::Result<renderer::RendererQualityPreset> parse_quality(std::string_view value) {
    if (value == "low") {
        return core::Result<renderer::RendererQualityPreset>::success(
            renderer::RendererQualityPreset::low);
    }
    if (value == "medium") {
        return core::Result<renderer::RendererQualityPreset>::success(
            renderer::RendererQualityPreset::medium);
    }
    if (value == "high") {
        return core::Result<renderer::RendererQualityPreset>::success(
            renderer::RendererQualityPreset::high);
    }
    if (value == "ultra") {
        return core::Result<renderer::RendererQualityPreset>::success(
            renderer::RendererQualityPreset::ultra);
    }
    return core::Result<renderer::RendererQualityPreset>::failure(
        "application_settings.invalid_quality", "unknown rendering quality preset");
}

[[nodiscard]] std::string color_vision_name(renderer::UiColorVisionMode mode) {
    switch (mode) {
    case renderer::UiColorVisionMode::none:
        return "none";
    case renderer::UiColorVisionMode::protanopia:
        return "protanopia";
    case renderer::UiColorVisionMode::deuteranopia:
        return "deuteranopia";
    case renderer::UiColorVisionMode::tritanopia:
        return "tritanopia";
    }
    return "none";
}

[[nodiscard]] core::Result<renderer::UiColorVisionMode> parse_color_vision(std::string_view value) {
    if (value == "none") {
        return core::Result<renderer::UiColorVisionMode>::success(
            renderer::UiColorVisionMode::none);
    }
    if (value == "protanopia") {
        return core::Result<renderer::UiColorVisionMode>::success(
            renderer::UiColorVisionMode::protanopia);
    }
    if (value == "deuteranopia") {
        return core::Result<renderer::UiColorVisionMode>::success(
            renderer::UiColorVisionMode::deuteranopia);
    }
    if (value == "tritanopia") {
        return core::Result<renderer::UiColorVisionMode>::success(
            renderer::UiColorVisionMode::tritanopia);
    }
    return core::Result<renderer::UiColorVisionMode>::failure(
        "application_settings.invalid_color_vision", "unknown color-vision mode");
}

template <typename Value>
[[nodiscard]] core::Result<Value> parse_number(std::string_view value, std::string_view field) {
    Value result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return core::Result<Value>::failure("application_settings.invalid_number",
                                            "invalid numeric setting: " + std::string(field));
    }
    return core::Result<Value>::success(result);
}

[[nodiscard]] core::Result<bool> parse_bool(std::string_view value, std::string_view field) {
    if (value == "true") {
        return core::Result<bool>::success(true);
    }
    if (value == "false") {
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::failure("application_settings.invalid_boolean",
                                       "invalid boolean setting: " + std::string(field));
}

} // namespace

core::Status ApplicationSettings::validate() const {
    if (window_width < 640 || window_width > 16'384 || window_height < 360 ||
        window_height > 16'384) {
        return core::Status::failure("application_settings.invalid_resolution",
                                     "window resolution is outside the supported range");
    }
    const auto unit = [](float value) { return value >= 0.0F && value <= 1.0F; };
    if (!unit(master_volume) || !unit(music_volume) || !unit(effects_volume) ||
        mouse_sensitivity < 0.1F || mouse_sensitivity > 10.0F || controller_sensitivity < 0.1F ||
        controller_sensitivity > 10.0F || ui_scale < 0.75F || ui_scale > 2.0F ||
        ui_contrast < 0.5F || ui_contrast > 2.0F || ui_saturation < 0.0F || ui_saturation > 2.0F) {
        return core::Status::failure("application_settings.invalid_range",
                                     "one or more application settings are outside bounds");
    }
    if (!safe_text(last_world_slot, 96) || recent_servers.size() > 16 ||
        std::ranges::any_of(recent_servers,
                            [](const std::string& server) { return !safe_text(server, 256); })) {
        return core::Status::failure("application_settings.invalid_history",
                                     "recent-world or server history is invalid");
    }
    return core::Status::ok();
}

ApplicationSettingsStore::ApplicationSettingsStore(std::filesystem::path path)
    : path_(std::move(path)) {}

const std::filesystem::path& ApplicationSettingsStore::path() const noexcept {
    return path_;
}

core::Result<ApplicationSettings> ApplicationSettingsStore::load() const {
    std::error_code error;
    if (!std::filesystem::exists(path_, error)) {
        if (error) {
            return core::Result<ApplicationSettings>::failure("application_settings.read_failed",
                                                              error.message());
        }
        return core::Result<ApplicationSettings>::success({});
    }
    const auto bytes = std::filesystem::file_size(path_, error);
    if (error || bytes > maximum_settings_bytes) {
        return core::Result<ApplicationSettings>::failure(
            "application_settings.read_failed",
            error ? error.message() : "application settings file is too large");
    }
    std::ifstream input(path_, std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line) || line != settings_magic) {
        return core::Result<ApplicationSettings>::failure("application_settings.invalid_header",
                                                          "application settings header is invalid");
    }
    ApplicationSettings settings;
    std::unordered_set<std::string> seen;
    bool ended = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "end") {
            ended = true;
            break;
        }
        const auto separator = line.find('|');
        if (separator == std::string::npos) {
            return core::Result<ApplicationSettings>::failure(
                "application_settings.invalid_line", "settings line is missing a separator");
        }
        const auto key = line.substr(0, separator);
        const auto value = std::string_view(line).substr(separator + 1);
        if (key != "recent_server" && !seen.emplace(key).second) {
            return core::Result<ApplicationSettings>::failure(
                "application_settings.duplicate_field", "duplicate application setting: " + key);
        }
        if (key == "window_width") {
            auto parsed = parse_number<std::uint32_t>(value, key);
            if (!parsed)
                return core::Result<ApplicationSettings>::failure(parsed.error().code,
                                                                  parsed.error().message);
            settings.window_width = parsed.value();
        } else if (key == "window_height") {
            auto parsed = parse_number<std::uint32_t>(value, key);
            if (!parsed)
                return core::Result<ApplicationSettings>::failure(parsed.error().code,
                                                                  parsed.error().message);
            settings.window_height = parsed.value();
        } else if (key == "windowed" || key == "controller_enabled" || key == "reduced_motion") {
            auto parsed = parse_bool(value, key);
            if (!parsed)
                return core::Result<ApplicationSettings>::failure(parsed.error().code,
                                                                  parsed.error().message);
            if (key == "windowed")
                settings.windowed = parsed.value();
            else if (key == "controller_enabled")
                settings.controller_enabled = parsed.value();
            else
                settings.reduced_motion = parsed.value();
        } else if (key == "rendering_quality") {
            auto parsed = parse_quality(value);
            if (!parsed)
                return core::Result<ApplicationSettings>::failure(parsed.error().code,
                                                                  parsed.error().message);
            settings.rendering_quality = parsed.value();
        } else if (key == "color_vision") {
            auto parsed = parse_color_vision(value);
            if (!parsed)
                return core::Result<ApplicationSettings>::failure(parsed.error().code,
                                                                  parsed.error().message);
            settings.color_vision_mode = parsed.value();
        } else if (key == "last_world_slot") {
            settings.last_world_slot = std::string(value);
        } else if (key == "recent_server") {
            settings.recent_servers.emplace_back(value);
        } else {
            auto parsed = parse_number<float>(value, key);
            if (!parsed)
                return core::Result<ApplicationSettings>::failure(parsed.error().code,
                                                                  parsed.error().message);
            if (key == "master_volume")
                settings.master_volume = parsed.value();
            else if (key == "music_volume")
                settings.music_volume = parsed.value();
            else if (key == "effects_volume")
                settings.effects_volume = parsed.value();
            else if (key == "mouse_sensitivity")
                settings.mouse_sensitivity = parsed.value();
            else if (key == "controller_sensitivity")
                settings.controller_sensitivity = parsed.value();
            else if (key == "ui_scale")
                settings.ui_scale = parsed.value();
            else if (key == "ui_contrast")
                settings.ui_contrast = parsed.value();
            else if (key == "ui_saturation")
                settings.ui_saturation = parsed.value();
            else
                return core::Result<ApplicationSettings>::failure(
                    "application_settings.unknown_field", "unknown application setting: " + key);
        }
    }
    if (!ended) {
        return core::Result<ApplicationSettings>::failure("application_settings.incomplete",
                                                          "application settings has no end marker");
    }
    auto status = settings.validate();
    if (!status) {
        return core::Result<ApplicationSettings>::failure(status.error().code,
                                                          status.error().message);
    }
    return core::Result<ApplicationSettings>::success(std::move(settings));
}

core::Status ApplicationSettingsStore::save(const ApplicationSettings& settings) const {
    auto status = settings.validate();
    if (!status) {
        return status;
    }
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
        return failure("application_settings.create_failed", error);
    }
    const auto temporary = path_.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return core::Status::failure("application_settings.write_failed",
                                         "failed to open temporary settings file");
        }
        output << settings_magic << '\n'
               << "window_width|" << settings.window_width << '\n'
               << "window_height|" << settings.window_height << '\n'
               << "windowed|" << (settings.windowed ? "true" : "false") << '\n'
               << "rendering_quality|" << quality_name(settings.rendering_quality) << '\n'
               << "master_volume|" << settings.master_volume << '\n'
               << "music_volume|" << settings.music_volume << '\n'
               << "effects_volume|" << settings.effects_volume << '\n'
               << "mouse_sensitivity|" << settings.mouse_sensitivity << '\n'
               << "controller_sensitivity|" << settings.controller_sensitivity << '\n'
               << "controller_enabled|" << (settings.controller_enabled ? "true" : "false") << '\n'
               << "ui_scale|" << settings.ui_scale << '\n'
               << "ui_contrast|" << settings.ui_contrast << '\n'
               << "ui_saturation|" << settings.ui_saturation << '\n'
               << "color_vision|" << color_vision_name(settings.color_vision_mode) << '\n'
               << "reduced_motion|" << (settings.reduced_motion ? "true" : "false") << '\n'
               << "last_world_slot|" << settings.last_world_slot << '\n';
        for (const auto& server : settings.recent_servers) {
            output << "recent_server|" << server << '\n';
        }
        output << "end\n";
        if (!output) {
            return core::Status::failure("application_settings.write_failed",
                                         "failed to write application settings");
        }
    }
    error = core::replace_file(temporary, path_);
    if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return failure("application_settings.rename_failed", error);
    }
    return core::Status::ok();
}

std::filesystem::path default_application_data_root() {
    if (const auto* configured = std::getenv("HEARTSTEAD_DATA_ROOT");
        configured != nullptr && *configured != '\0') {
        return std::filesystem::path(configured);
    }
#if defined(_WIN32)
    if (const auto* app_data = std::getenv("APPDATA"); app_data != nullptr && *app_data != '\0') {
        return std::filesystem::path(app_data) / "Heartstead";
    }
#elif defined(__APPLE__)
    if (const auto* user_home = std::getenv("HOME"); user_home != nullptr && *user_home != '\0') {
        return std::filesystem::path(user_home) / "Library" / "Application Support" / "Heartstead";
    }
#else
    if (const auto* xdg_data = std::getenv("XDG_DATA_HOME");
        xdg_data != nullptr && *xdg_data != '\0') {
        return std::filesystem::path(xdg_data) / "heartstead";
    }
    if (const auto* user_home = std::getenv("HOME"); user_home != nullptr && *user_home != '\0') {
        return std::filesystem::path(user_home) / ".local" / "share" / "heartstead";
    }
#endif
    return std::filesystem::current_path() / ".heartstead";
}

} // namespace heartstead::game
