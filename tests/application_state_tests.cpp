#include "game/application/application_state.hpp"

#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using namespace heartstead;

class RecordingLifecycle final : public game::IApplicationStateLifecycle {
  public:
    core::Status enter_state(game::ApplicationState state,
                             const game::ApplicationTransition&) override {
        entered.push_back(state);
        if (fail_enter == state) {
            partially_entered = state;
            return core::Status::failure("test.enter_failed", "injected state entry failure");
        }
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
        if (partially_entered == state) {
            partially_entered.reset();
        }
        if (fail_exit == state) {
            return core::Status::failure("test.exit_failed", "injected state exit failure");
        }
        return core::Status::ok();
    }

    std::vector<game::ApplicationState> entered;
    std::vector<game::ApplicationState> updated;
    std::vector<game::ApplicationState> exited;
    std::optional<game::ApplicationState> fail_enter;
    std::optional<game::ApplicationState> fail_exit;
    std::optional<game::ApplicationState> partially_entered;
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
    assert(!game::authoritative_simulation_advances(states.policy(), false));
    assert(game::authoritative_simulation_advances(states.policy(), true));
    assert(states.policy().world_rendering);
    assert(states.transition(game::ApplicationState::in_game, "resume requested"));
    assert(states.transition(game::ApplicationState::session_unloading, "return to menu"));
    assert(states.transition(game::ApplicationState::main_menu, "session released"));
    assert(states.transition(game::ApplicationState::shutdown, "quit selected"));
    assert(states.history().size() == 9);
    assert(lifecycle.entered.front() == game::ApplicationState::boot);
    assert(lifecycle.exited.back() == game::ApplicationState::main_menu);
}

void test_failed_entry_compensates_target_and_restores_previous_state() {
    RecordingLifecycle lifecycle;
    game::ApplicationStateMachine states(&lifecycle);
    assert(states.start());
    assert(states.transition(game::ApplicationState::main_menu, "boot complete"));
    const auto history_before = states.history().size();
    lifecycle.fail_enter = game::ApplicationState::session_loading;
    const auto status =
        states.transition(game::ApplicationState::session_loading, "injected failed load");
    assert(!status);
    assert(status.error().code == "test.enter_failed");
    assert(states.state() == game::ApplicationState::main_menu);
    assert(!lifecycle.partially_entered.has_value());
    assert(lifecycle.exited.back() == game::ApplicationState::session_loading);
    assert(lifecycle.entered.back() == game::ApplicationState::main_menu);
    assert(states.history().size() == history_before + 1);
    assert(states.history().back().from == game::ApplicationState::session_loading);
    assert(states.history().back().to == game::ApplicationState::main_menu);
}

void test_failed_entry_surfaces_compensation_failure() {
    RecordingLifecycle lifecycle;
    game::ApplicationStateMachine states(&lifecycle);
    assert(states.start());
    assert(states.transition(game::ApplicationState::main_menu, "boot complete"));
    lifecycle.fail_enter = game::ApplicationState::session_loading;
    lifecycle.fail_exit = game::ApplicationState::session_loading;
    const auto status =
        states.transition(game::ApplicationState::session_loading, "injected failed rollback");
    assert(!status);
    assert(status.error().code == "application_state.rollback_failed");
    assert(status.error().message.contains("target-state cleanup failed"));
    assert(states.state() == game::ApplicationState::main_menu);
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
    assert(!game::authoritative_simulation_advances(menu, false));
    assert(!game::authoritative_simulation_advances(menu, true));
    assert(!menu.world_rendering);

    const auto& in_game = game::application_state_policy(game::ApplicationState::in_game);
    assert(in_game.input_owner == game::ApplicationInputOwner::session);
    assert(in_game.cursor_owner == game::ApplicationCursorOwner::captured);
    assert(in_game.session_presence == game::SessionPresence::required);
    assert(game::authoritative_simulation_advances(in_game, false));
    assert(game::authoritative_simulation_advances(in_game, true));
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
    test_failed_entry_compensates_target_and_restores_previous_state();
    test_failed_entry_surfaces_compensation_failure();
    test_error_recovery_retains_diagnostic();
    test_error_states_require_error_payloads();
    test_state_policies_define_ownership();
    return 0;
}
