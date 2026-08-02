#include "pixel_ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::render::ui {

struct UiColor {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
    std::uint8_t alpha{255};
};

[[nodiscard]] std::array<std::uint8_t, 7> glyph(char character) noexcept {
    switch (character) {
    case 'A': return {14, 17, 17, 31, 17, 17, 17};
    case 'B': return {30, 17, 17, 30, 17, 17, 30};
    case 'C': return {15, 16, 16, 16, 16, 16, 15};
    case 'D': return {30, 17, 17, 17, 17, 17, 30};
    case 'E': return {31, 16, 16, 30, 16, 16, 31};
    case 'F': return {31, 16, 16, 30, 16, 16, 16};
    case 'G': return {15, 16, 16, 23, 17, 17, 15};
    case 'H': return {17, 17, 17, 31, 17, 17, 17};
    case 'I': return {31, 4, 4, 4, 4, 4, 31};
    case 'J': return {7, 2, 2, 2, 18, 18, 12};
    case 'K': return {17, 18, 20, 24, 20, 18, 17};
    case 'L': return {16, 16, 16, 16, 16, 16, 31};
    case 'M': return {17, 27, 21, 21, 17, 17, 17};
    case 'N': return {17, 25, 21, 19, 17, 17, 17};
    case 'O': return {14, 17, 17, 17, 17, 17, 14};
    case 'P': return {30, 17, 17, 30, 16, 16, 16};
    case 'Q': return {14, 17, 17, 17, 21, 18, 13};
    case 'R': return {30, 17, 17, 30, 20, 18, 17};
    case 'S': return {15, 16, 16, 14, 1, 1, 30};
    case 'T': return {31, 4, 4, 4, 4, 4, 4};
    case 'U': return {17, 17, 17, 17, 17, 17, 14};
    case 'V': return {17, 17, 17, 17, 17, 10, 4};
    case 'W': return {17, 17, 17, 21, 21, 27, 17};
    case 'X': return {17, 17, 10, 4, 10, 17, 17};
    case 'Y': return {17, 17, 10, 4, 4, 4, 4};
    case 'Z': return {31, 1, 2, 4, 8, 16, 31};
    case '0': return {14, 17, 19, 21, 25, 17, 14};
    case '1': return {4, 12, 4, 4, 4, 4, 14};
    case '2': return {14, 17, 1, 2, 4, 8, 31};
    case '3': return {30, 1, 1, 14, 1, 1, 30};
    case '4': return {2, 6, 10, 18, 31, 2, 2};
    case '5': return {31, 16, 16, 30, 1, 1, 30};
    case '6': return {14, 16, 16, 30, 17, 17, 14};
    case '7': return {31, 1, 2, 4, 8, 8, 8};
    case '8': return {14, 17, 17, 14, 17, 17, 14};
    case '9': return {14, 17, 17, 15, 1, 1, 14};
    case '.': return {0, 0, 0, 0, 0, 6, 6};
    case ':': return {0, 6, 6, 0, 6, 6, 0};
    case '%': return {24, 25, 2, 4, 8, 19, 3};
    case '-': return {0, 0, 0, 31, 0, 0, 0};
    case '+': return {0, 4, 4, 31, 4, 4, 0};
    case '_': return {0, 0, 0, 0, 0, 0, 31};
    case '/': return {1, 2, 2, 4, 8, 8, 16};
    case '!': return {4, 4, 4, 4, 4, 0, 4};
    default: return {};
    }
}

void fill_rectangle(
    std::vector<std::uint8_t>& pixels,
    std::int32_t width,
    std::int32_t height,
    std::int32_t left,
    std::int32_t top,
    std::int32_t right,
    std::int32_t bottom,
    UiColor color);

void draw_text(
    std::vector<std::uint8_t>& pixels,
    std::int32_t width,
    std::int32_t height,
    std::int32_t x,
    std::int32_t y,
    std::string_view text,
    std::int32_t scale,
    UiColor color);

void build_menu_pixels(
    std::vector<std::uint8_t>& pixels,
    std::int32_t width,
    std::int32_t height,
    const MenuUiState& state,
    const VideoSettings& settings) {
    pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0U);
    fill_rectangle(pixels, width, height, 0, 0, width, height, {4, 9, 11, 72});
    const auto layout = MenuLayout::from_framebuffer(state.screen, width, height);

    const auto centered_text = [&](UiRectangle rectangle, std::string_view label,
                                   std::int32_t scale, UiColor color) {
        const auto text_width = static_cast<std::int32_t>(label.size()) * 6 * scale;
        const auto text_height = 7 * scale;
        draw_text(pixels, width, height,
            (rectangle.left + rectangle.right - text_width) / 2,
            (rectangle.top + rectangle.bottom - text_height) / 2,
            label, scale, color);
    };
    const auto button = [&](UiRectangle rectangle, std::string_view label, std::int32_t control,
                            bool enabled) {
        const auto hovered = state.hovered_control == control && enabled;
        fill_rectangle(pixels, width, height, rectangle.left - 2, rectangle.top - 2,
            rectangle.right + 2, rectangle.bottom + 2,
            hovered ? UiColor{108, 226, 146, 255} : UiColor{81, 102, 108, 235});
        fill_rectangle(pixels, width, height, rectangle.left, rectangle.top,
            rectangle.right, rectangle.bottom,
            enabled
                ? (hovered ? UiColor{35, 91, 61, 248} : UiColor{24, 36, 41, 238})
                : UiColor{28, 32, 34, 218});
        centered_text(rectangle, label, 2,
            enabled ? UiColor{235, 246, 239, 255} : UiColor{112, 122, 123, 255});
    };

    if (state.screen == MenuScreen::main) {
        fill_rectangle(pixels, width, height, layout.panel.left, layout.panel.top,
            layout.panel.right, layout.panel.bottom, {7, 15, 18, 218});
        fill_rectangle(pixels, width, height, layout.panel.left, 0,
            layout.panel.left + 6, height, {77, 205, 124, 220});
        draw_text(pixels, width, height, layout.panel.left + 40, 78,
            "HEARTSTEAD", 5, {230, 247, 235, 255});
        draw_text(pixels, width, height, layout.panel.left + 43, 132,
            "VOXEL FRONTIER", 2, {94, 218, 139, 255});
        draw_text(pixels, width, height, layout.panel.left + 43, 174,
            "BUILD BEYOND THE HORIZON", 1, {170, 189, 184, 255});
        button(layout.primary, "SINGLEPLAYER", 0, true);
        button(layout.secondary, "MULTIPLAYER", 1, true);
        button(layout.tertiary, "SETTINGS", 2, true);
        button(layout.quaternary, "QUIT", 3, true);
        draw_text(pixels, width, height, layout.panel.left + 43, height - 42,
            "ENGINE 0.1  C++23  OPENGL", 1, {103, 127, 125, 255});
        return;
    }

    if (state.screen == MenuScreen::pause) {
        fill_rectangle(pixels, width, height, 0, 0, width, height, {2, 5, 7, 168});
        fill_rectangle(pixels, width, height, layout.panel.left, layout.panel.top,
            layout.panel.right, layout.panel.bottom, {14, 23, 28, 250});
        fill_rectangle(pixels, width, height, layout.panel.left, layout.panel.top,
            layout.panel.right, layout.panel.top + 70, {27, 47, 53, 255});
        centered_text({layout.panel.left, layout.panel.top,
            layout.panel.right, layout.panel.top + 70},
            "PAUSED", 3, {235, 247, 239, 255});
        button(layout.primary, "SETTINGS", 0, true);
        button(layout.secondary, "MAIN MENU", 1, true);
        centered_text({layout.panel.left, layout.panel.bottom - 34,
            layout.panel.right, layout.panel.bottom},
            "ESC TO RESUME", 1, {126, 153, 151, 255});
        return;
    }

    fill_rectangle(pixels, width, height, 0, 0, width, height, {3, 7, 9, 116});
    fill_rectangle(pixels, width, height, layout.panel.left, layout.panel.top,
        layout.panel.right, layout.panel.bottom, {15, 24, 29, 246});
    fill_rectangle(pixels, width, height, layout.panel.left, layout.panel.top,
        layout.panel.right, layout.panel.top + 70, {24, 42, 48, 255});

    if (state.screen == MenuScreen::singleplayer_worlds) {
        draw_text(pixels, width, height, layout.panel.left + 34, layout.panel.top + 22,
            "SINGLEPLAYER WORLDS", 3, {233, 246, 237, 255});
        if (state.saved_worlds.empty()) {
            centered_text({layout.panel.left + 42, layout.panel.top + 110,
                layout.panel.right - 42, layout.panel.bottom - 110},
                "NO SAVED WORLDS YET", 2, {139, 160, 158, 255});
        }
        const auto shown_worlds = std::min(state.saved_worlds.size(), layout.world_tabs.size());
        for (std::size_t index = 0; index < shown_worlds; ++index) {
            const auto& entry = state.saved_worlds[index];
            const auto rectangle = layout.world_tabs[index];
            const auto selected = state.selected_world == static_cast<std::int32_t>(index);
            const auto hovered = state.hovered_control == 10 + static_cast<std::int32_t>(index);
            fill_rectangle(pixels, width, height, rectangle.left - 2, rectangle.top - 2,
                rectangle.right + 2, rectangle.bottom + 2,
                selected ? UiColor{91, 224, 139, 255}
                         : (hovered ? UiColor{101, 137, 130, 255} : UiColor{50, 68, 71, 255}));
            fill_rectangle(pixels, width, height, rectangle.left, rectangle.top,
                rectangle.right, rectangle.bottom,
                selected ? UiColor{26, 68, 47, 248} : UiColor{10, 19, 22, 244});
            draw_text(pixels, width, height, rectangle.left + 14, rectangle.top + 10,
                entry.name, 2, {229, 241, 233, 255});
            draw_text(pixels, width, height, rectangle.left + 14, rectangle.top + 34,
                "CREATED " + entry.created, 1, {124, 150, 147, 255});
            const auto played = "PLAYED " + entry.last_played;
            draw_text(pixels, width, height,
                rectangle.right - 14 - static_cast<std::int32_t>(played.size()) * 6,
                rectangle.top + 34, played, 1, {124, 150, 147, 255});
        }
        const auto can_play = state.selected_world >= 0 &&
            static_cast<std::size_t>(state.selected_world) < state.saved_worlds.size();
        button(layout.primary, "PLAY", 0, can_play);
        button(layout.secondary, "CREATE NEW", 1, true);
        button(layout.tertiary, "BACK", 2, true);
        return;
    }

    if (state.screen == MenuScreen::create_world) {
        draw_text(pixels, width, height, layout.panel.left + 34, layout.panel.top + 22,
            "CREATE A WORLD", 3, {233, 246, 237, 255});
        draw_text(pixels, width, height, layout.text_field.left, layout.text_field.top - 32,
            "WORLD NAME", 2, {137, 213, 162, 255});
        fill_rectangle(pixels, width, height, layout.text_field.left - 2, layout.text_field.top - 2,
            layout.text_field.right + 2, layout.text_field.bottom + 2,
            state.editing_world_name ? UiColor{94, 223, 139, 255} : UiColor{67, 85, 89, 255});
        fill_rectangle(pixels, width, height, layout.text_field.left, layout.text_field.top,
            layout.text_field.right, layout.text_field.bottom, {8, 15, 18, 255});
        draw_text(pixels, width, height, layout.text_field.left + 16, layout.text_field.top + 15,
            state.world_name, 2, {232, 242, 235, 255});
        if (state.editing_world_name) {
            const auto cursor_x = layout.text_field.left + 16 +
                static_cast<std::int32_t>(state.world_name.size()) * 12;
            fill_rectangle(pixels, width, height, cursor_x, layout.text_field.top + 11,
                cursor_x + 3, layout.text_field.bottom - 11, {110, 233, 151, 255});
        }

        const auto info_x = layout.panel.left + 56;
        const auto info_y = layout.text_field.bottom + 42;
        draw_text(pixels, width, height, info_x, info_y,
            "CREATED", 2, {126, 148, 150, 255});
        draw_text(pixels, width, height, info_x + 190, info_y,
            state.creation_date, 2, {222, 232, 228, 255});
        draw_text(pixels, width, height, info_x, info_y + 38,
            "GAME MODE", 2, {126, 148, 150, 255});
        draw_text(pixels, width, height, info_x + 190, info_y + 38,
            "SURVIVAL", 2, {222, 232, 228, 255});
        draw_text(pixels, width, height, info_x, info_y + 76,
            "WORLD TYPE", 2, {126, 148, 150, 255});
        draw_text(pixels, width, height, info_x + 190, info_y + 76,
            "CONTINENTAL", 2, {222, 232, 228, 255});
        draw_text(pixels, width, height, info_x, info_y + 114,
            "SEED", 2, {126, 148, 150, 255});
        draw_text(pixels, width, height, info_x + 190, info_y + 114,
            "PROCEDURAL", 2, {222, 232, 228, 255});
        draw_text(pixels, width, height, info_x, info_y + 152,
            "VIEW DISTANCE", 2, {126, 148, 150, 255});
        draw_text(pixels, width, height, info_x + 190, info_y + 152,
            std::to_string(settings.render_distance_chunks) + " CHUNKS", 2,
            {222, 232, 228, 255});
        button(layout.primary, "CREATE WORLD", 0, !state.world_name.empty());
        button(layout.secondary, "BACK", 1, true);
        return;
    }

    draw_text(pixels, width, height, layout.panel.left + 34, layout.panel.top + 22,
        "MULTIPLAYER", 3, {233, 246, 237, 255});
    draw_text(pixels, width, height, layout.panel.left + 54, layout.panel.top + 130,
        "SERVER BROWSER", 2, {111, 217, 148, 255});
    fill_rectangle(pixels, width, height, layout.panel.left + 54, layout.panel.top + 174,
        layout.panel.right - 54, layout.panel.bottom - 126, {8, 14, 17, 230});
    centered_text({layout.panel.left + 54, layout.panel.top + 174,
        layout.panel.right - 54, layout.panel.bottom - 126},
        state.multiplayer_status, 1, {160, 179, 177, 255});
    button(layout.primary, "REFRESH", 0, true);
    button(layout.secondary, "BACK", 1, true);
}

void fill_rectangle(
    std::vector<std::uint8_t>& pixels,
    std::int32_t width,
    std::int32_t height,
    std::int32_t left,
    std::int32_t top,
    std::int32_t right,
    std::int32_t bottom,
    UiColor color) {
    left = std::clamp(left, 0, width);
    right = std::clamp(right, 0, width);
    top = std::clamp(top, 0, height);
    bottom = std::clamp(bottom, 0, height);
    for (auto y = top; y < bottom; ++y) {
        for (auto x = left; x < right; ++x) {
            const auto index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x)) * 4U;
            pixels[index] = color.red;
            pixels[index + 1U] = color.green;
            pixels[index + 2U] = color.blue;
            pixels[index + 3U] = color.alpha;
        }
    }
}

void draw_text(
    std::vector<std::uint8_t>& pixels,
    std::int32_t width,
    std::int32_t height,
    std::int32_t x,
    std::int32_t y,
    std::string_view text,
    std::int32_t scale,
    UiColor color) {
    const auto start_x = x;
    for (const auto character : text) {
        if (character == '\n') {
            x = start_x;
            y += 9 * scale;
            continue;
        }
        const auto rows = glyph(character);
        for (std::int32_t row = 0; row < 7; ++row) {
            for (std::int32_t column = 0; column < 5; ++column) {
                if ((rows[static_cast<std::size_t>(row)] & (1U << (4 - column))) != 0U) {
                    fill_rectangle(pixels, width, height,
                        x + column * scale, y + row * scale,
                        x + (column + 1) * scale, y + (row + 1) * scale, color);
                }
            }
        }
        x += 6 * scale;
    }
}

void build_settings_pixels(
    std::vector<std::uint8_t>& pixels,
    std::int32_t width,
    std::int32_t height,
    const VideoSettings& settings,
    const VideoSettingsUiState& ui_state) {
    pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0U);
    fill_rectangle(pixels, width, height, 0, 0, width, height, {5, 8, 12, 170});
    const auto layout = VideoSettingsLayout::from_framebuffer(width, height);
    fill_rectangle(pixels, width, height, layout.panel_left, layout.panel_top,
        layout.panel_right, layout.panel_bottom, {20, 27, 36, 248});
    fill_rectangle(pixels, width, height, layout.panel_left, layout.panel_top,
        layout.panel_right, layout.panel_top + 58, {30, 43, 56, 255});
    draw_text(pixels, width, height, layout.panel_left + 28, layout.panel_top + 18,
        "SETTINGS", 3, {235, 244, 248, 255});
    draw_text(pixels, width, height, layout.panel_left + 30, layout.panel_top + 78,
        "VIDEO SETTINGS", 2, {91, 204, 132, 255});

    draw_text(pixels, width, height, layout.slider_left, layout.render_distance_y - 48,
        "RENDER DISTANCE", 2, {226, 234, 239, 255});
    fill_rectangle(pixels, width, height, layout.render_value_left, layout.render_value_top,
        layout.render_value_right, layout.render_value_bottom,
        ui_state.editing_render_distance ? UiColor{38, 73, 58, 255} : UiColor{29, 39, 49, 255});
    const auto render_value = ui_state.editing_render_distance
        ? ui_state.numeric_text
        : std::to_string(settings.render_distance_chunks) + " CHUNKS";
    draw_text(pixels, width, height, layout.render_value_left + 10,
        layout.render_value_top + 9, render_value, 2, {188, 226, 201, 255});
    if (ui_state.editing_render_distance) {
        const auto cursor_x = layout.render_value_left + 10 + static_cast<std::int32_t>(render_value.size()) * 12;
        fill_rectangle(pixels, width, height, cursor_x, layout.render_value_top + 7,
            cursor_x + 3, layout.render_value_bottom - 7, {226, 249, 234, 255});
    }

    draw_text(pixels, width, height, layout.slider_left, layout.smoothing_y - 48,
        "DISTANCE SMOOTHING", 2, {226, 234, 239, 255});
    const auto smoothing_value = std::to_string(static_cast<std::int32_t>(settings.distance_smoothing_start)) + " BLOCKS";
    draw_text(pixels, width, height, layout.slider_right - static_cast<std::int32_t>(smoothing_value.size()) * 12,
        layout.smoothing_y - 48, smoothing_value, 2, {155, 176, 190, 255});

    draw_text(pixels, width, height, layout.slider_left, layout.fog_start_y - 48,
        "FOG START", 2, {226, 234, 239, 255});
    const auto fog_value = std::to_string(static_cast<std::int32_t>(
        std::round(settings.fog_start_fraction * 100.0F))) + "% OF DISTANCE";
    draw_text(pixels, width, height, layout.slider_right - static_cast<std::int32_t>(fog_value.size()) * 12,
        layout.fog_start_y - 48, fog_value, 2, {155, 176, 190, 255});

    draw_text(pixels, width, height, layout.slider_left, layout.shadow_distance_y - 48,
        "SHADOW DISTANCE", 2, {226, 234, 239, 255});
    const auto shadow_value = std::to_string(settings.shadow_distance_blocks) + " BLOCKS";
    draw_text(pixels, width, height, layout.slider_right - static_cast<std::int32_t>(shadow_value.size()) * 12,
        layout.shadow_distance_y - 48, shadow_value, 2, {155, 176, 190, 255});

    const auto draw_slider = [&](std::int32_t y, float value) {
        fill_rectangle(pixels, width, height, layout.slider_left, y - 4, layout.slider_right, y + 4,
            {59, 73, 84, 255});
        const auto knob = layout.slider_left + static_cast<std::int32_t>(
            std::clamp(value, 0.0F, 1.0F) * static_cast<float>(layout.slider_right - layout.slider_left));
        fill_rectangle(pixels, width, height, layout.slider_left, y - 4, knob, y + 4,
            {62, 201, 116, 255});
        fill_rectangle(pixels, width, height, knob - 7, y - 12, knob + 7, y + 12,
            {220, 248, 229, 255});
    };
    draw_slider(layout.render_distance_y,
        static_cast<float>(settings.render_distance_chunks - 4) /
            static_cast<float>(std::max(1, settings.render_distance_scale_max - 4)));
    draw_slider(layout.smoothing_y, (settings.distance_smoothing_start - 128.0F) / 512.0F);
    draw_slider(layout.fog_start_y, (settings.fog_start_fraction - 0.55F) / 0.37F);
    draw_slider(layout.shadow_distance_y,
        static_cast<float>(settings.shadow_distance_blocks - 64) / 192.0F);

    const auto draw_toggle = [&](std::int32_t y, std::string_view label, bool enabled) {
        draw_text(pixels, width, height, layout.slider_left, y - 8,
            label, 2, {226, 234, 239, 255});
        fill_rectangle(pixels, width, height, layout.toggle_left, y - 16,
            layout.toggle_right, y + 16,
            enabled ? UiColor{38, 104, 66, 255} : UiColor{46, 57, 67, 255});
        const std::string_view value = enabled ? "ON" : "OFF";
        const auto value_width = static_cast<std::int32_t>(value.size()) * 12;
        draw_text(pixels, width, height,
            (layout.toggle_left + layout.toggle_right - value_width) / 2,
            y - 8, value, 2,
            enabled ? UiColor{205, 249, 220, 255} : UiColor{171, 183, 191, 255});
    };
    draw_toggle(layout.vsync_y, "VSYNC", settings.vsync);
    draw_toggle(layout.fullscreen_y, "FULLSCREEN", settings.fullscreen);

    draw_text(pixels, width, height, layout.panel_left + 30, layout.panel_bottom - 38,
        "ESC TO RETURN", 2, {138, 157, 170, 255});
    draw_text(pixels, width, height, layout.panel_left + 210, layout.panel_bottom - 38,
        "DOUBLE CLICK VALUE", 2, {96, 126, 141, 255});
}

[[nodiscard]] std::string debug_text(const DebugStats& stats) {
    constexpr double bytes_per_megabyte = 1024.0 * 1024.0;
    char text[768]{};
    std::snprintf(text, sizeof(text),
        "FPS %.0f\n"
        "CPU FRAME %.2f MS\n"
        "CPU USE %.1f %%\n"
        "GPU FRAME %.2f MS\n"
        "GPU LOAD %.1f %%\n"
        "WORLD RAM %.1f MB\n"
        "GPU MESH %.1f MB\n"
        "CHUNKS %u\n"
        "VISIBLE %u\n"
        "OCCLUDED %u",
        stats.frames_per_second,
        stats.cpu_frame_milliseconds,
        stats.cpu_usage_percent,
        stats.gpu_frame_milliseconds,
        stats.gpu_load_percent,
        static_cast<double>(stats.world_storage_bytes) / bytes_per_megabyte,
        static_cast<double>(stats.gpu_storage_bytes) / bytes_per_megabyte,
        stats.total_chunks,
        stats.visible_chunks,
        stats.occluded_chunks);
    return text;
}

void build_debug_pixels(
    std::vector<std::uint8_t>& pixels,
    std::int32_t width,
    std::int32_t height,
    std::string_view stats_text) {
    pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 0U);
    fill_rectangle(pixels, width, height, 14, 14, 424, 276, {9, 14, 19, 210});
    fill_rectangle(pixels, width, height, 14, 14, 424, 52, {27, 42, 53, 238});
    draw_text(pixels, width, height, 28, 25, "HEARTSTEAD DEBUG", 2, {103, 224, 145, 255});
    draw_text(pixels, width, height, 28, 66, stats_text, 2, {231, 239, 243, 255});
}



} // namespace heartstead::render::ui
