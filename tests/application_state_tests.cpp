#include "game/application/application_state.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

using namespace heartstead;

class RecordingLifecycle final : public game::IApplicationStateLifecycle {
  public:
    core::Status enter_state(game::ApplicationState state,
                             const game::ApplicationTransition&) override {
        entered.push_back(state);
        return core::Status::ok();
    }

    core::Status update_state(game::ApplicationState state,
                              std::uint64_t delta_microseconds) override {
        updated.push_back(state);
        last_delta_microseconds = delta_microseconds;
        return core::Status::ok();
    }

    core::Status exit_state(game::ApplicationState state,
                            const game::ApplicationTransition&) override {
        exited.push_back(state);
        return core::Status::ok();
    }

    std::vector<game::ApplicationState> entered;
    std::vector<game::ApplicationState> updated;
    std::vector<game::ApplicationState> exited;
    std::uint64_t last_delta_microseconds = 0;
};

void test_normal_application_lifecycle() {
    RecordingLifecycle lifecycle;
    game::ApplicationStateMachine states(&lifecycle);
    assert(states.start());
    assert(states.state() == game::ApplicationState::boot);
    assert(states.transition(game::ApplicationState::main_menu, "boot complete"));
    assert(states.transition(game::ApplicationState::session_loading, "launch local world"));
    assert(states.transition(game::ApplicationState::in_game, "local session ready"));
    assert(states.update(16'667));
    assert(lifecycle.last_delta_microseconds == 16'667);
    assert(states.transition(game::ApplicationState::paused, "pause requested"));
    assert(!states.policy().authoritative_simulation_advances);
    assert(states.policy().world_rendering);
    assert(states.transition(game::ApplicationState::in_game, "resume requested"));
    assert(states.transition(game::ApplicationState::session_unloading, "return to menu"));
    assert(states.transition(game::ApplicationState::main_menu, "session released"));
    assert(states.transition(game::ApplicationState::shutdown, "quit selected"));
    assert(states.history().size() == 9);
    assert(lifecycle.entered.front() == game::ApplicationState::boot);
    assert(lifecycle.exited.back() == game::ApplicationState::main_menu);
}

void test_invalid_transitions_are_rejected_without_callbacks() {
    RecordingLifecycle lifecycle;
    game::ApplicationStateMachine states(&lifecycle);
    assert(states.start());
    const auto entered_before = lifecycle.entered.size();
    const auto exited_before = lifecycle.exited.size();
    const auto history_before = states.history().size();
    auto status = states.transition(game::ApplicationState::in_game, "skip loading");
    assert(!status);
    assert(status.error().code == "application_state.invalid_transition");
    assert(states.state() == game::ApplicationState::boot);
    assert(lifecycle.entered.size() == entered_before);
    assert(lifecycle.exited.size() == exited_before);
    assert(states.history().size() == history_before);
}

void test_error_recovery_retains_diagnostic() {
    game::ApplicationStateMachine states;
    assert(states.start());
    assert(states.transition(game::ApplicationState::main_menu, "boot complete"));
    assert(states.transition(game::ApplicationState::session_loading, "load save"));
    const core::Error failure{"save.invalid", "save snapshot is corrupt"};
    assert(states.transition(game::ApplicationState::load_failure, "save load failed", failure));
    assert(states.active_error().has_value());
    assert(states.active_error()->code == "save.invalid");
    assert(states.transition(game::ApplicationState::main_menu, "failure acknowledged"));
    assert(!states.active_error().has_value());
}

void test_error_states_require_error_payloads() {
    game::ApplicationStateMachine states;
    assert(states.start());
    assert(states.transition(game::ApplicationState::main_menu, "boot complete"));
    assert(states.transition(game::ApplicationState::session_loading, "connect"));
    auto status = states.transition(game::ApplicationState::connection_failure,
                                    "connection failed without details");
    assert(!status);
    assert(status.error().code == "application_state.missing_error");
    assert(states.state() == game::ApplicationState::session_loading);
}

void test_state_policies_define_ownership() {
    const auto& menu = game::application_state_policy(game::ApplicationState::main_menu);
    assert(menu.input_owner == game::ApplicationInputOwner::menu);
    assert(menu.cursor_owner == game::ApplicationCursorOwner::released);
    assert(menu.ui_owner == game::ApplicationUiOwner::menu);
    assert(menu.session_presence == game::SessionPresence::forbidden);
    assert(!menu.authoritative_simulation_advances);
    assert(!menu.world_rendering);

    const auto& in_game = game::application_state_policy(game::ApplicationState::in_game);
    assert(in_game.input_owner == game::ApplicationInputOwner::session);
    assert(in_game.cursor_owner == game::ApplicationCursorOwner::captured);
    assert(in_game.session_presence == game::SessionPresence::required);
    assert(in_game.authoritative_simulation_advances);
    assert(in_game.world_rendering);

    for (std::uint8_t value = 0;
         value <= static_cast<std::uint8_t>(game::ApplicationState::shutdown); ++value) {
        const auto state = static_cast<game::ApplicationState>(value);
        assert(game::application_state_policy(state).platform_events);
    }
}

} // namespace

int main() {
    test_normal_application_lifecycle();
    test_invalid_transitions_are_rejected_without_callbacks();
    test_error_recovery_retains_diagnostic();
    test_error_states_require_error_payloads();
    test_state_policies_define_ownership();
    return 0;
}
