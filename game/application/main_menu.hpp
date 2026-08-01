#pragma once

#include "engine/core/result.hpp"
#include "engine/net/transport.hpp"

#include <cstdint>
#include <string>
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
    rename_world,
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
[[nodiscard]] core::Result<std::string> normalize_world_display_name(std::string_view name);
[[nodiscard]] std::string world_slot_id(std::string_view normalized_display_name);
[[nodiscard]] core::Result<std::uint64_t> parse_world_seed(std::string_view text);
[[nodiscard]] core::Result<net::TransportEndpoint>
parse_server_endpoint(std::string_view text);

} // namespace heartstead::game
