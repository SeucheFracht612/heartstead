#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace heartstead {

enum class MenuScreen { main, singleplayer_worlds, create_world, multiplayer, pause };

struct SavedWorldUiEntry {
    std::string id;
    std::string name;
    std::string created;
    std::string last_played;

    bool operator==(const SavedWorldUiEntry&) const = default;
};

struct UiRectangle {
    std::int32_t left{};
    std::int32_t top{};
    std::int32_t right{};
    std::int32_t bottom{};

    [[nodiscard]] bool contains(double x, double y) const noexcept {
        return x >= static_cast<double>(left) && x <= static_cast<double>(right) &&
            y >= static_cast<double>(top) && y <= static_cast<double>(bottom);
    }
};

struct MenuUiState {
    MenuScreen screen{MenuScreen::main};
    std::string world_name{"NEW WORLD"};
    std::string creation_date;
    std::string multiplayer_status{"ONLINE SERVICES ARE NOT CONNECTED"};
    std::vector<SavedWorldUiEntry> saved_worlds;
    std::int32_t selected_world{-1};
    bool editing_world_name{};
    std::int32_t hovered_control{-1};
};

struct MenuLayout {
    UiRectangle panel;
    UiRectangle text_field;
    UiRectangle primary;
    UiRectangle secondary;
    UiRectangle tertiary;
    UiRectangle quaternary;
    std::array<UiRectangle, 5> world_tabs{};

    [[nodiscard]] static MenuLayout from_framebuffer(
        MenuScreen screen, std::int32_t width, std::int32_t height) noexcept;
};

} // namespace heartstead
