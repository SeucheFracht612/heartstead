#pragma once

#include "engine/core/result.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::game {

enum class ApplicationState : std::uint8_t {
    boot,
    main_menu,
    session_loading,
    in_game,
    paused,
    session_unloading,
    load_failure,
    connection_failure,
    fatal_error,
    shutdown,
};

enum class ApplicationInputOwner : std::uint8_t {
    application,
    menu,
    session,
};

enum class ApplicationCursorOwner : std::uint8_t {
    released,
    captured,
};

enum class ApplicationUiOwner : std::uint8_t {
    boot,
    menu,
    loading,
    session,
    pause,
    error,
    none,
};

enum class SessionPresence : std::uint8_t {
    forbidden,
    optional,
    required,
};

enum class AuthoritativeSimulationPolicy : std::uint8_t {
    stopped,
    always,
    multiplayer_only,
};

struct ApplicationStatePolicy {
    ApplicationInputOwner input_owner = ApplicationInputOwner::application;
    ApplicationCursorOwner cursor_owner = ApplicationCursorOwner::released;
    ApplicationUiOwner ui_owner = ApplicationUiOwner::none;
    SessionPresence session_presence = SessionPresence::forbidden;
    AuthoritativeSimulationPolicy authoritative_simulation =
        AuthoritativeSimulationPolicy::stopped;
    bool world_rendering = false;
    bool platform_events = true;
    bool application_audio = true;
};

struct ApplicationTransition {
    std::uint64_t sequence = 0;
    ApplicationState from = ApplicationState::boot;
    ApplicationState to = ApplicationState::boot;
    std::string reason;
    std::optional<core::Error> error;
};

class IApplicationStateLifecycle {
  public:
    virtual ~IApplicationStateLifecycle() = default;

    [[nodiscard]] virtual core::Status enter_state(ApplicationState state,
                                                   const ApplicationTransition& transition) = 0;
    [[nodiscard]] virtual core::Status update_state(ApplicationState state,
                                                    std::uint64_t delta_microseconds) = 0;
    [[nodiscard]] virtual core::Status exit_state(ApplicationState state,
                                                  const ApplicationTransition& transition) = 0;
};

class ApplicationStateMachine final {
  public:
    explicit ApplicationStateMachine(IApplicationStateLifecycle* lifecycle = nullptr) noexcept;

    [[nodiscard]] core::Status start(std::string reason = "application boot");
    [[nodiscard]] core::Status update(std::uint64_t delta_microseconds);
    [[nodiscard]] core::Status transition(ApplicationState next, std::string reason,
                                          std::optional<core::Error> error = std::nullopt);

    [[nodiscard]] ApplicationState state() const noexcept;
    [[nodiscard]] const ApplicationStatePolicy& policy() const noexcept;
    [[nodiscard]] const std::optional<core::Error>& active_error() const noexcept;
    [[nodiscard]] std::span<const ApplicationTransition> history() const noexcept;
    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] bool can_transition_to(ApplicationState next) const noexcept;

    void attach_lifecycle(IApplicationStateLifecycle* lifecycle) noexcept;

  private:
    IApplicationStateLifecycle* lifecycle_ = nullptr;
    ApplicationState state_ = ApplicationState::boot;
    std::optional<core::Error> active_error_;
    std::vector<ApplicationTransition> history_;
    std::uint64_t next_sequence_ = 1;
    bool started_ = false;
};

[[nodiscard]] const ApplicationStatePolicy&
application_state_policy(ApplicationState state) noexcept;
[[nodiscard]] bool is_valid_application_transition(ApplicationState from,
                                                   ApplicationState to) noexcept;
[[nodiscard]] std::string_view application_state_name(ApplicationState state) noexcept;
[[nodiscard]] std::string_view application_input_owner_name(ApplicationInputOwner owner) noexcept;
[[nodiscard]] std::string_view application_cursor_owner_name(ApplicationCursorOwner owner) noexcept;
[[nodiscard]] std::string_view application_ui_owner_name(ApplicationUiOwner owner) noexcept;
[[nodiscard]] std::string_view session_presence_name(SessionPresence presence) noexcept;
[[nodiscard]] bool authoritative_simulation_advances(const ApplicationStatePolicy& policy,
                                                     bool multiplayer) noexcept;

} // namespace heartstead::game
