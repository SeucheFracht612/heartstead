#include "game/application/main_menu.hpp"

#include "engine/core/hash.hpp"
#include "engine/core/text.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <random>

namespace heartstead::game {

MainMenuScreen MainMenuNavigation::screen() const noexcept {
    return screen_;
}

core::Status MainMenuNavigation::open(MainMenuScreen screen) {
    const auto valid =
        screen_ == MainMenuScreen::root
            ? screen != MainMenuScreen::delete_confirmation &&
                  screen != MainMenuScreen::rename_world
        : screen_ == MainMenuScreen::load_world
            ? screen == MainMenuScreen::root || screen == MainMenuScreen::delete_confirmation ||
                  screen == MainMenuScreen::rename_world
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
    screen_ = screen_ == MainMenuScreen::delete_confirmation ||
                      screen_ == MainMenuScreen::rename_world
                  ? MainMenuScreen::load_world
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
    case MainMenuScreen::rename_world:
        return "rename-world";
    case MainMenuScreen::delete_confirmation:
        return "delete-confirmation";
    }
    return "unknown";
}

core::Result<std::string> normalize_world_display_name(std::string_view name) {
    name = core::trim_ascii_whitespace(name);
    if (!core::is_valid_utf8_text(name, 96, "|")) {
        return core::Result<std::string>::failure(
            "heartstead.invalid_world_name",
            "world name must be 1-96 bytes of printable UTF-8 without the '|' character");
    }
    return core::Result<std::string>::success(std::string(name));
}

std::string world_slot_id(std::string_view normalized_display_name) {
    std::string slug;
    bool separator = false;
    for (const auto character : normalized_display_name) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x80U && std::isalnum(byte) != 0) {
            slug.push_back(static_cast<char>(std::tolower(byte)));
            separator = false;
        } else if (!slug.empty() && !separator) {
            slug.push_back('_');
            separator = true;
        }
        if (slug.size() == 48) {
            break;
        }
    }
    while (!slug.empty() && slug.back() == '_') {
        slug.pop_back();
    }
    if (slug.empty()) {
        slug = "world";
    }
    return slug + "_" + core::stable_hash64_hex(normalized_display_name).substr(0, 12);
}

core::Result<std::uint64_t> parse_world_seed(std::string_view text) {
    text = core::trim_ascii_whitespace(text);
    if (text.empty()) {
        std::random_device random;
        const auto high = static_cast<std::uint64_t>(random()) << 32U;
        const auto low = static_cast<std::uint64_t>(random());
        const auto seed = high | low;
        return core::Result<std::uint64_t>::success(seed == 0 ? 1 : seed);
    }
    if (text.starts_with('-')) {
        std::int64_t signed_seed = 0;
        const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), signed_seed, 10);
        if (error == std::errc{} && end == text.data() + text.size()) {
            return core::Result<std::uint64_t>::success(static_cast<std::uint64_t>(signed_seed));
        }
    } else {
        auto digits = text;
        auto base = 10;
        if (text.starts_with("0x") || text.starts_with("0X")) {
            digits.remove_prefix(2);
            base = 16;
        }
        std::uint64_t numeric_seed = 0;
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), numeric_seed, base);
        if (!digits.empty() && error == std::errc{} && end == digits.data() + digits.size()) {
            return core::Result<std::uint64_t>::success(numeric_seed);
        }
    }
    if (!core::is_valid_utf8_text(text, 256)) {
        return core::Result<std::uint64_t>::failure(
            "heartstead.invalid_world_seed",
            "world seed must be blank, an integer, hexadecimal, or printable UTF-8 text");
    }
    return core::Result<std::uint64_t>::success(core::stable_hash64(text));
}

core::Result<net::TransportEndpoint> parse_server_endpoint(std::string_view text) {
    text = core::trim_ascii_whitespace(text);
    std::string_view host;
    std::string_view port_text;
    if (text.starts_with('[')) {
        const auto closing = text.find(']');
        if (closing == std::string_view::npos || closing == 1 || closing + 2 > text.size() ||
            text[closing + 1] != ':') {
            return core::Result<net::TransportEndpoint>::failure(
                "heartstead.invalid_server_address",
                "IPv6 server addresses use the form [address]:port");
        }
        host = text.substr(1, closing - 1);
        port_text = text.substr(closing + 2);
    } else {
        const auto separator = text.rfind(':');
        if (separator == std::string_view::npos || separator == 0 ||
            separator + 1 >= text.size() || text.find(':') != separator) {
            return core::Result<net::TransportEndpoint>::failure(
                "heartstead.invalid_server_address",
                "server address uses host:port or [IPv6-address]:port");
        }
        host = text.substr(0, separator);
        port_text = text.substr(separator + 1);
    }
    std::uint16_t port = 0;
    const auto [end, error] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (error != std::errc{} || end != port_text.data() + port_text.size() || port == 0) {
        return core::Result<net::TransportEndpoint>::failure(
            "heartstead.invalid_server_port", "server port must be between 1 and 65535");
    }
    net::TransportEndpoint endpoint{std::string(host), port};
    auto status = net::validate_transport_endpoint(endpoint);
    return !status ? core::Result<net::TransportEndpoint>::failure(status.error().code,
                                                                   status.error().message)
                   : core::Result<net::TransportEndpoint>::success(std::move(endpoint));
}

} // namespace heartstead::game
