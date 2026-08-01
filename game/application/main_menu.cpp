#include "game/application/main_menu.hpp"

namespace heartstead::game {

MainMenuScreen MainMenuNavigation::screen() const noexcept {
    return screen_;
}

core::Status MainMenuNavigation::open(MainMenuScreen screen) {
    const auto valid =
        screen_ == MainMenuScreen::root ? screen != MainMenuScreen::delete_confirmation
        : screen_ == MainMenuScreen::load_world
            ? screen == MainMenuScreen::root || screen == MainMenuScreen::delete_confirmation
            : screen == MainMenuScreen::root;
    if (!valid) {
        return core::Status::failure("main_menu.invalid_navigation",
                                     "requested menu screen is not reachable from current screen");
    }
    screen_ = screen;
    return core::Status::ok();
}

bool MainMenuNavigation::back() noexcept {
    if (screen_ == MainMenuScreen::root) {
        return false;
    }
    screen_ = screen_ == MainMenuScreen::delete_confirmation ? MainMenuScreen::load_world
                                                             : MainMenuScreen::root;
    return true;
}

void MainMenuNavigation::reset() noexcept {
    screen_ = MainMenuScreen::root;
}

std::vector<MainMenuActionState> MainMenuNavigation::root_actions(bool can_continue) {
    return {
        {MainMenuAction::continue_world, can_continue,
         can_continue ? std::string_view{} : std::string_view{"No compatible recent world"}},
        {MainMenuAction::new_world, true, {}},
        {MainMenuAction::load_world, true, {}},
        {MainMenuAction::multiplayer, true, {}},
        {MainMenuAction::developer_worlds, true, {}},
        {MainMenuAction::options, true, {}},
        {MainMenuAction::quit, true, {}},
    };
}

std::string_view main_menu_screen_name(MainMenuScreen screen) noexcept {
    switch (screen) {
    case MainMenuScreen::root:
        return "root";
    case MainMenuScreen::new_world:
        return "new-world";
    case MainMenuScreen::load_world:
        return "load-world";
    case MainMenuScreen::multiplayer:
        return "multiplayer";
    case MainMenuScreen::developer_worlds:
        return "developer-worlds";
    case MainMenuScreen::options:
        return "options";
    case MainMenuScreen::delete_confirmation:
        return "delete-confirmation";
    }
    return "unknown";
}

} // namespace heartstead::game
