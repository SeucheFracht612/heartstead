#pragma once

#include "engine/core/result.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace heartstead::game {

enum class InitialLaunchKind {
    scenario,
    world,
    new_world,
    connect,
    host,
};

struct InitialLaunchDirective {
    InitialLaunchKind kind = InitialLaunchKind::scenario;
    std::string target;
    std::optional<std::uint64_t> seed;
};

struct HeartsteadLaunchOptions {
    bool headless = false;
    bool safe_mode = false;
    bool show_help = false;
    bool show_version = false;
    std::optional<std::uint64_t> maximum_frames;
    std::optional<InitialLaunchDirective> initial_launch;
};

[[nodiscard]] core::Result<HeartsteadLaunchOptions>
parse_heartstead_launch_options(std::span<const std::string_view> arguments);
[[nodiscard]] std::string heartstead_command_line_usage(std::string_view executable);
[[nodiscard]] std::string_view initial_launch_kind_name(InitialLaunchKind kind) noexcept;

} // namespace heartstead::game
