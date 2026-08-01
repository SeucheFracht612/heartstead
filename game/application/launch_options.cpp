#include "game/application/launch_options.hpp"

#include <charconv>
#include <sstream>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] core::Result<std::uint64_t> parse_unsigned(std::string_view value,
                                                         std::string_view label,
                                                         bool allow_zero) {
    std::uint64_t result = 0;
    const auto [end, error] =
        std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() ||
        (!allow_zero && result == 0)) {
        return core::Result<std::uint64_t>::failure(
            "heartstead.invalid_numeric_option",
            std::string(label) + (allow_zero ? " must be an unsigned 64-bit integer"
                                             : " must be a positive 64-bit integer"));
    }
    return core::Result<std::uint64_t>::success(result);
}

[[nodiscard]] core::Status set_launch(HeartsteadLaunchOptions& options, InitialLaunchKind kind,
                                      std::string_view target) {
    if (target.empty()) {
        return core::Status::failure("heartstead.empty_launch_target",
                                     std::string(initial_launch_kind_name(kind)) +
                                         " requires a non-empty value");
    }
    if (options.initial_launch.has_value()) {
        return core::Status::failure(
            "heartstead.conflicting_launch_options",
            "only one of --scenario, --world, --new-world, --connect, or --host may be used");
    }
    options.initial_launch = InitialLaunchDirective{kind, std::string(target), std::nullopt};
    return core::Status::ok();
}

} // namespace

core::Result<HeartsteadLaunchOptions>
parse_heartstead_launch_options(std::span<const std::string_view> arguments) {
    HeartsteadLaunchOptions options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        const auto require_value = [&]() -> core::Result<std::string_view> {
            if (index + 1 >= arguments.size()) {
                return core::Result<std::string_view>::failure(
                    "heartstead.missing_option_value", std::string(argument) + " requires a value");
            }
            return core::Result<std::string_view>::success(arguments[++index]);
        };
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
        } else if (argument == "--version") {
            options.show_version = true;
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--safe-mode") {
            options.safe_mode = true;
        } else if (argument == "--frames" || argument == "--native-frames") {
            auto value = require_value();
            if (!value) {
                return core::Result<HeartsteadLaunchOptions>::failure(value.error().code,
                                                                       value.error().message);
            }
            auto frames = parse_unsigned(value.value(), "frame count", false);
            if (!frames) {
                return core::Result<HeartsteadLaunchOptions>::failure(frames.error().code,
                                                                       frames.error().message);
            }
            options.maximum_frames = frames.value();
            if (argument == "--frames") {
                options.headless = true;
            }
        } else if (argument == "--seed") {
            auto value = require_value();
            if (!value) {
                return core::Result<HeartsteadLaunchOptions>::failure(value.error().code,
                                                                       value.error().message);
            }
            auto seed = parse_unsigned(value.value(), "seed", true);
            if (!seed) {
                return core::Result<HeartsteadLaunchOptions>::failure(seed.error().code,
                                                                       seed.error().message);
            }
            if (options.initial_launch.has_value()) {
                options.initial_launch->seed = seed.value();
            } else {
                options.initial_launch =
                    InitialLaunchDirective{InitialLaunchKind::new_world, {}, seed.value()};
            }
        } else {
            std::optional<InitialLaunchKind> kind;
            if (argument == "--scenario")
                kind = InitialLaunchKind::scenario;
            else if (argument == "--world")
                kind = InitialLaunchKind::world;
            else if (argument == "--new-world")
                kind = InitialLaunchKind::new_world;
            else if (argument == "--connect")
                kind = InitialLaunchKind::connect;
            else if (argument == "--host")
                kind = InitialLaunchKind::host;
            if (!kind.has_value()) {
                return core::Result<HeartsteadLaunchOptions>::failure(
                    "heartstead.unknown_option", "unknown option: " + std::string(argument));
            }
            auto value = require_value();
            if (!value) {
                return core::Result<HeartsteadLaunchOptions>::failure(value.error().code,
                                                                       value.error().message);
            }
            const auto pending_seed = options.initial_launch.has_value() &&
                                              options.initial_launch->target.empty()
                                          ? options.initial_launch->seed
                                          : std::nullopt;
            if (options.initial_launch.has_value() && !options.initial_launch->target.empty()) {
                return core::Result<HeartsteadLaunchOptions>::failure(
                    "heartstead.conflicting_launch_options",
                    "only one automatic session launch may be requested");
            }
            options.initial_launch.reset();
            auto status = set_launch(options, *kind, value.value());
            if (!status) {
                return core::Result<HeartsteadLaunchOptions>::failure(status.error().code,
                                                                       status.error().message);
            }
            options.initial_launch->seed = pending_seed;
        }
    }
    if (options.initial_launch.has_value()) {
        const auto& launch = *options.initial_launch;
        if (launch.target.empty()) {
            return core::Result<HeartsteadLaunchOptions>::failure(
                "heartstead.seed_without_world",
                "--seed must be combined with --new-world NAME or --scenario ID");
        }
        if (launch.seed.has_value() && launch.kind != InitialLaunchKind::new_world &&
            launch.kind != InitialLaunchKind::scenario) {
            return core::Result<HeartsteadLaunchOptions>::failure(
                "heartstead.seed_not_supported",
                "--seed is supported only with --new-world or --scenario");
        }
    }
    return core::Result<HeartsteadLaunchOptions>::success(std::move(options));
}

std::string heartstead_command_line_usage(std::string_view executable) {
    std::ostringstream output;
    output << "Usage: " << executable << " [OPTIONS]\n\n"
           << "Session launch (choose at most one):\n"
           << "  --scenario ID       Launch a registered developer world\n"
           << "  --world PATH_OR_ID  Load a save slot or explicit save directory\n"
           << "  --new-world NAME    Create and launch a persistent world\n"
           << "  --connect ADDRESS   Connect to HOST:PORT\n"
           << "  --host PATH_OR_ID   Host a save slot or explicit save directory\n"
           << "  --seed N            Seed for --new-world or --scenario\n\n"
           << "Application:\n"
           << "  --safe-mode         Use conservative renderer/session settings\n"
           << "  --headless          Run without a native window or renderer\n"
           << "  --frames N          Headless bounded smoke run\n"
           << "  --native-frames N   Native bounded smoke run\n"
           << "  -h, --help          Show this help\n"
           << "  --version           Show the Heartstead version\n";
    return output.str();
}

std::string_view initial_launch_kind_name(InitialLaunchKind kind) noexcept {
    switch (kind) {
    case InitialLaunchKind::scenario:
        return "scenario";
    case InitialLaunchKind::world:
        return "world";
    case InitialLaunchKind::new_world:
        return "new world";
    case InitialLaunchKind::connect:
        return "connection";
    case InitialLaunchKind::host:
        return "host";
    }
    return "unknown";
}

} // namespace heartstead::game
