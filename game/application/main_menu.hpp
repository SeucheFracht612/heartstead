#pragma once

#include "engine/core/result.hpp"

#include <string_view>
#include <vector>

namespace heartstead::game {

enum class MainMenuScreen {
    root,
    new_world,
    load_world,
    multiplayer,
    developer_worlds,
    options,
    delete_confirmation,
};

enum class MainMenuAction {
    continue_world,
    new_world,
    load_world,
    multiplayer,
    developer_worlds,
    options,
    quit,
};

struct MainMenuActionState {
    MainMenuAction action = MainMenuAction::continue_world;
    bool enabled = true;
    std::string_view disabled_reason;
};

class MainMenuNavigation final {
  public:
    [[nodiscard]] MainMenuScreen screen() const noexcept;
    [[nodiscard]] core::Status open(MainMenuScreen screen);
    [[nodiscard]] bool back() noexcept;
    void reset() noexcept;

    [[nodiscard]] static std::vector<MainMenuActionState> root_actions(bool can_continue);

  private:
    MainMenuScreen screen_ = MainMenuScreen::root;
};

[[nodiscard]] std::string_view main_menu_screen_name(MainMenuScreen screen) noexcept;

} // namespace heartstead::game
