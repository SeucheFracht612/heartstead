#include "engine/content/content_validation.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/runtime/game_runtime.hpp"

#include <cassert>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace heartstead;

game::SessionLaunchRequest make_local_request(const content::ContentValidationReport& report,
                                              std::uint64_t generation = 0) {
    auto metadata = content::save_metadata_from_content_report(report, "runtime-lifecycle-test",
                                                               game::foundation::world_seed);
    assert(metadata);
    game::SessionLaunchRequest request;
    request.ownership_generation = generation;
    request.mode = game::SessionMode::local_single_player;
    request.world_source = game::WorldSourceKind::developer_scenario;
    request.persistence = game::PersistencePolicy::ephemeral;
    request.world_name = "Lifecycle fixture";
    request.scenario_id = game::foundation::scenario_id;
    request.seed = game::foundation::world_seed;
    request.metadata = std::move(metadata).value();
    request.runtime.headless = true;
    return request;
}

game::GameRuntime make_runtime(const content::ContentValidationReport& report) {
    auto runtime = game::GameRuntime::initialize({}, report);
    assert(runtime);
    return std::move(runtime).value();
}

void test_launch_descriptor_validation(const content::ContentValidationReport& report) {
    auto request = make_local_request(report);
    assert(request.validate());
    assert(game::session_mode_name(request.mode) == "local-single-player");
    assert(game::world_source_kind_name(request.world_source) == "developer-scenario");
    assert(game::persistence_policy_name(request.persistence) == "ephemeral");

    request.mode = game::SessionMode::remote_multiplayer;
    auto status = request.validate();
    assert(!status && status.error().code == "session_launch.remote_endpoint_missing");
    request.mode = game::SessionMode::replay;
    status = request.validate();
    assert(!status && status.error().code == "session_launch.replay_unsupported");
}

void test_local_teardown_and_replacement(const content::ContentValidationReport& report) {
    auto runtime = make_runtime(report);
    assert(runtime.start_session(make_local_request(report, 10)));
    auto* first = runtime.session();
    assert(first != nullptr);
    assert(first->ownership_generation() == 10);
    assert(first->state() == game::RuntimeSessionState::running);
    assert(first->connection_state() == game::SessionConnectionState::connected);
    assert(first->server() != nullptr && first->client() != nullptr);

    std::vector<std::string> cleanup_order;
    assert(first->register_cleanup("first", [&cleanup_order]() {
        cleanup_order.emplace_back("first");
        return core::Status::ok();
    }));
    assert(first->register_cleanup("second", [&cleanup_order]() {
        cleanup_order.emplace_back("second");
        return core::Status::ok();
    }));
    assert(first->request_stop());
    assert(!first->accepts_commands());
    assert(first->state() == game::RuntimeSessionState::stopping);
    auto command = first->submit_command("world.set_voxel", {});
    assert(!command && command.error().code == "runtime_session.command_path_unavailable");
    assert(runtime.shutdown());
    assert(runtime.session() == nullptr);
    assert((cleanup_order == std::vector<std::string>{"second", "first"}));
    assert(runtime.last_teardown_report().has_value());
    const auto& teardown = *runtime.last_teardown_report();
    assert(teardown.ownership_generation == 10);
    assert(teardown.rejected_new_commands);
    assert(teardown.transport_stopped);
    assert(teardown.authoritative_ticking_stopped);
    assert(teardown.presentation_cleared);
    assert(teardown.presentation_objects_after == 0);
    assert(teardown.completed_cleanup_count == 2);
    assert(teardown.client_destroyed && teardown.server_destroyed);

    auto stale = make_local_request(report, 9);
    auto status = runtime.start_session(std::move(stale));
    assert(!status && status.error().code == "game_runtime.stale_session_generation");

    assert(runtime.start_session(make_local_request(report, 11)));
    assert(runtime.session()->ownership_generation() == 11);
    assert(runtime.run_frame({16'667, 17}));
    assert(runtime.shutdown());

    assert(runtime.start_session(make_local_request(report, 12)));
    assert(runtime.session()->ownership_generation() == 12);
    assert(runtime.shutdown());
}

void test_remote_attempt_cancels_and_recovers(const content::ContentValidationReport& report) {
    auto runtime = make_runtime(report);
    auto request = make_local_request(report, 20);
    request.mode = game::SessionMode::remote_multiplayer;
    request.world_source = game::WorldSourceKind::remote_server;
    request.network_endpoint = net::TransportEndpoint{"127.0.0.1", 47991};
    assert(runtime.start_session(std::move(request)));
    assert(runtime.session()->server() == nullptr);
    assert(runtime.session()->client() != nullptr);
    assert(runtime.session()->connection_state() == game::SessionConnectionState::connecting);
    assert(runtime.session()->request_stop());
    assert(runtime.shutdown());
    assert(runtime.last_teardown_report()->transport_stopped);

    assert(runtime.start_session(make_local_request(report, 21)));
    assert(runtime.session()->connection_state() == game::SessionConnectionState::connected);
    assert(runtime.shutdown());
}

} // namespace

int main() {
    const auto report =
        content::ContentValidation::validate(std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR));
    assert(!report.has_errors());
    test_launch_descriptor_validation(report);
    test_local_teardown_and_replacement(report);
    test_remote_attempt_cancels_and_recovers(report);
    return 0;
}
