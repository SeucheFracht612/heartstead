#include "game/application/application_state.hpp"

#include "engine/core/logging.hpp"

#include <array>
#include <utility>

namespace heartstead::game {

namespace {

constexpr std::array<ApplicationStatePolicy, 10> policies{{
    // boot
    {ApplicationInputOwner::application, ApplicationCursorOwner::released, ApplicationUiOwner::boot,
     SessionPresence::forbidden, false, false, true, true},
    // main_menu
    {ApplicationInputOwner::menu, ApplicationCursorOwner::released, ApplicationUiOwner::menu,
     SessionPresence::forbidden, false, false, true, true},
    // session_loading
    {ApplicationInputOwner::menu, ApplicationCursorOwner::released, ApplicationUiOwner::loading,
     SessionPresence::optional, false, false, true, true},
    // in_game
    {ApplicationInputOwner::session, ApplicationCursorOwner::captured, ApplicationUiOwner::session,
     SessionPresence::required, true, true, true, true},
    // paused
    {ApplicationInputOwner::menu, ApplicationCursorOwner::released, ApplicationUiOwner::pause,
     SessionPresence::required, false, true, true, true},
    // session_unloading
    {ApplicationInputOwner::application, ApplicationCursorOwner::released,
     ApplicationUiOwner::loading, SessionPresence::optional, false, false, true, true},
    // load_failure
    {ApplicationInputOwner::menu, ApplicationCursorOwner::released, ApplicationUiOwner::error,
     SessionPresence::forbidden, false, false, true, true},
    // connection_failure
    {ApplicationInputOwner::menu, ApplicationCursorOwner::released, ApplicationUiOwner::error,
     SessionPresence::forbidden, false, false, true, true},
    // fatal_error
    {ApplicationInputOwner::application, ApplicationCursorOwner::released,
     ApplicationUiOwner::error, SessionPresence::optional, false, false, true, false},
    // shutdown
    {ApplicationInputOwner::application, ApplicationCursorOwner::released, ApplicationUiOwner::none,
     SessionPresence::optional, false, false, true, false},
}};

[[nodiscard]] constexpr std::size_t state_index(ApplicationState state) noexcept {
    return static_cast<std::size_t>(state);
}

} // namespace

ApplicationStateMachine::ApplicationStateMachine(IApplicationStateLifecycle* lifecycle) noexcept
    : lifecycle_(lifecycle) {}

core::Status ApplicationStateMachine::start(std::string reason) {
    if (started_) {
        return core::Status::failure("application_state.already_started",
                                     "application state machine has already started");
    }
    ApplicationTransition transition;
    transition.sequence = next_sequence_++;
    transition.from = ApplicationState::boot;
    transition.to = ApplicationState::boot;
    transition.reason = std::move(reason);
    if (lifecycle_ != nullptr) {
        auto status = lifecycle_->enter_state(ApplicationState::boot, transition);
        if (!status) {
            return status;
        }
    }
    started_ = true;
    history_.push_back(std::move(transition));
    core::log(core::LogLevel::info, "Application state #" +
                                        std::to_string(history_.back().sequence) +
                                        " entered Boot: " + history_.back().reason);
    return core::Status::ok();
}

core::Status ApplicationStateMachine::update(std::uint64_t delta_microseconds) {
    if (!started_) {
        return core::Status::failure("application_state.not_started",
                                     "application state machine must be started before update");
    }
    if (lifecycle_ == nullptr) {
        return core::Status::ok();
    }
    return lifecycle_->update_state(state_, delta_microseconds);
}

core::Status ApplicationStateMachine::transition(ApplicationState next, std::string reason,
                                                 std::optional<core::Error> error) {
    if (!started_) {
        return core::Status::failure("application_state.not_started",
                                     "application state machine must be started before transition");
    }
    if (reason.empty()) {
        return core::Status::failure("application_state.missing_reason",
                                     "application transitions require a diagnostic reason");
    }
    if (!is_valid_application_transition(state_, next)) {
        return core::Status::failure("application_state.invalid_transition",
                                     "invalid application transition from " +
                                         std::string(application_state_name(state_)) + " to " +
                                         std::string(application_state_name(next)));
    }
    if ((next == ApplicationState::load_failure || next == ApplicationState::connection_failure ||
         next == ApplicationState::fatal_error) &&
        !error.has_value()) {
        return core::Status::failure("application_state.missing_error",
                                     "application error states require an error payload");
    }

    ApplicationTransition transition;
    transition.sequence = next_sequence_++;
    transition.from = state_;
    transition.to = next;
    transition.reason = std::move(reason);
    transition.error = std::move(error);

    if (lifecycle_ != nullptr) {
        auto status = lifecycle_->exit_state(state_, transition);
        if (!status) {
            return status;
        }
        status = lifecycle_->enter_state(next, transition);
        if (!status) {
            const auto enter_error = status.error();
            ApplicationTransition rollback;
            rollback.sequence = next_sequence_++;
            rollback.from = next;
            rollback.to = state_;
            rollback.reason = "rollback after failed state entry";
            rollback.error = enter_error;
            (void)lifecycle_->enter_state(state_, rollback);
            return core::Status::failure(enter_error.code, enter_error.message);
        }
    }

    state_ = next;
    active_error_ = transition.error;
    if (next == ApplicationState::main_menu || next == ApplicationState::in_game ||
        next == ApplicationState::shutdown) {
        active_error_.reset();
    }
    history_.push_back(std::move(transition));
    const auto& completed = history_.back();
    core::log(completed.error.has_value() ? core::LogLevel::warning : core::LogLevel::info,
              "Application state #" + std::to_string(completed.sequence) + " " +
                  std::string(application_state_name(completed.from)) + " -> " +
                  std::string(application_state_name(completed.to)) + ": " + completed.reason +
                  (completed.error.has_value()
                       ? " [" + completed.error->code + ": " + completed.error->message + "]"
                       : std::string{}));
    return core::Status::ok();
}

ApplicationState ApplicationStateMachine::state() const noexcept {
    return state_;
}

const ApplicationStatePolicy& ApplicationStateMachine::policy() const noexcept {
    return application_state_policy(state_);
}

const std::optional<core::Error>& ApplicationStateMachine::active_error() const noexcept {
    return active_error_;
}

std::span<const ApplicationTransition> ApplicationStateMachine::history() const noexcept {
    return history_;
}

bool ApplicationStateMachine::started() const noexcept {
    return started_;
}

bool ApplicationStateMachine::can_transition_to(ApplicationState next) const noexcept {
    return started_ && is_valid_application_transition(state_, next);
}

void ApplicationStateMachine::attach_lifecycle(IApplicationStateLifecycle* lifecycle) noexcept {
    lifecycle_ = lifecycle;
}

const ApplicationStatePolicy& application_state_policy(ApplicationState state) noexcept {
    const auto index = state_index(state);
    return index < policies.size() ? policies[index] : policies.back();
}

bool is_valid_application_transition(ApplicationState from, ApplicationState to) noexcept {
    if (from == to) {
        return false;
    }
    if (to == ApplicationState::shutdown) {
        return from != ApplicationState::shutdown;
    }
    if (to == ApplicationState::fatal_error) {
        return from != ApplicationState::fatal_error && from != ApplicationState::shutdown;
    }
    switch (from) {
    case ApplicationState::boot:
        return to == ApplicationState::main_menu;
    case ApplicationState::main_menu:
        return to == ApplicationState::session_loading;
    case ApplicationState::session_loading:
        return to == ApplicationState::in_game || to == ApplicationState::session_unloading ||
               to == ApplicationState::load_failure || to == ApplicationState::connection_failure;
    case ApplicationState::in_game:
        return to == ApplicationState::paused || to == ApplicationState::session_unloading ||
               to == ApplicationState::load_failure || to == ApplicationState::connection_failure;
    case ApplicationState::paused:
        return to == ApplicationState::in_game || to == ApplicationState::session_unloading ||
               to == ApplicationState::load_failure || to == ApplicationState::connection_failure;
    case ApplicationState::session_unloading:
        return to == ApplicationState::main_menu || to == ApplicationState::load_failure ||
               to == ApplicationState::connection_failure;
    case ApplicationState::load_failure:
    case ApplicationState::connection_failure:
        return to == ApplicationState::main_menu || to == ApplicationState::session_loading;
    case ApplicationState::fatal_error:
    case ApplicationState::shutdown:
        return false;
    }
    return false;
}

std::string_view application_state_name(ApplicationState state) noexcept {
    constexpr std::array names{
        "Boot",       "MainMenu",         "SessionLoading", "InGame",
        "Paused",     "SessionUnloading", "LoadFailure",    "ConnectionFailure",
        "FatalError", "Shutdown"};
    const auto index = state_index(state);
    return index < names.size() ? names[index] : "Unknown";
}

std::string_view application_input_owner_name(ApplicationInputOwner owner) noexcept {
    switch (owner) {
    case ApplicationInputOwner::application:
        return "application";
    case ApplicationInputOwner::menu:
        return "menu";
    case ApplicationInputOwner::session:
        return "session";
    }
    return "unknown";
}

std::string_view application_cursor_owner_name(ApplicationCursorOwner owner) noexcept {
    return owner == ApplicationCursorOwner::captured ? "captured" : "released";
}

std::string_view application_ui_owner_name(ApplicationUiOwner owner) noexcept {
    switch (owner) {
    case ApplicationUiOwner::boot:
        return "boot";
    case ApplicationUiOwner::menu:
        return "menu";
    case ApplicationUiOwner::loading:
        return "loading";
    case ApplicationUiOwner::session:
        return "session";
    case ApplicationUiOwner::pause:
        return "pause";
    case ApplicationUiOwner::error:
        return "error";
    case ApplicationUiOwner::none:
        return "none";
    }
    return "unknown";
}

std::string_view session_presence_name(SessionPresence presence) noexcept {
    switch (presence) {
    case SessionPresence::forbidden:
        return "forbidden";
    case SessionPresence::optional:
        return "optional";
    case SessionPresence::required:
        return "required";
    }
    return "unknown";
}

} // namespace heartstead::game
