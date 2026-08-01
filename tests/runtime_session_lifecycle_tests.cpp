#include "engine/content/content_validation.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/runtime/game_runtime.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <string>
#include <stop_token>
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
    assert(!game::session_mode_is_multiplayer(request.mode));
    request.initial_runtime_time_ms = -1;
    auto status = request.validate();
    assert(!status && status.error().code == "session_launch.invalid_initial_time");
    request.initial_runtime_time_ms = 0;

    request.mode = game::SessionMode::remote_multiplayer;
    status = request.validate();
    assert(!status && status.error().code == "session_launch.remote_endpoint_missing");
    assert(game::session_mode_is_multiplayer(request.mode));
    request.mode = game::SessionMode::replay;
    status = request.validate();
    assert(!status && status.error().code == "session_launch.replay_unsupported");
}

void test_startup_phases_are_real_runtime_boundaries(
    const content::ContentValidationReport& report) {
    auto runtime = make_runtime(report);
    std::vector<game::SessionStartupPhase> phases;
    assert(runtime.start_session(
        make_local_request(report, 5),
        [&phases](game::SessionStartupPhase phase) { phases.push_back(phase); }));
    assert(!phases.empty());
    assert(phases.front() == game::SessionStartupPhase::validating_request);
    assert(std::ranges::find(phases, game::SessionStartupPhase::preparing_world) != phases.end());
    assert(std::ranges::find(phases, game::SessionStartupPhase::initializing_physics) !=
           phases.end());
    assert(std::ranges::find(phases, game::SessionStartupPhase::generating_spawn_area) !=
           phases.end());
    assert(std::ranges::find(phases, game::SessionStartupPhase::registering_gameplay_systems) !=
           phases.end());
    assert(std::ranges::find(phases, game::SessionStartupPhase::starting_authoritative_server) !=
           phases.end());
    assert(std::ranges::find(phases, game::SessionStartupPhase::starting_client) != phases.end());
    assert(std::ranges::find(phases, game::SessionStartupPhase::connecting_transport) !=
           phases.end());
    assert(std::ranges::find(phases, game::SessionStartupPhase::constructing_presentation) !=
           phases.end());
    assert(phases.back() == game::SessionStartupPhase::ready);
    assert(game::session_startup_phase_name(phases.back()) == "World ready");
    assert(runtime.shutdown());
}

void test_startup_cancellation_is_cooperative(
    const content::ContentValidationReport& report) {
    auto runtime = make_runtime(report);
    std::stop_source cancellation;
    cancellation.request_stop();
    auto status = runtime.start_session(make_local_request(report, 6), {},
                                        cancellation.get_token());
    assert(!status);
    assert(status.error().code == "session_startup.cancelled");
    assert(runtime.session() == nullptr);

    world::ChunkDatabase chunks;
    auto built = game::foundation::build_world(chunks, report.voxel_palette,
                                               cancellation.get_token());
    assert(!built);
    assert(built.error().code == "session_startup.cancelled");
    assert(chunks.chunk_count() == 0);
}

void test_application_runtime_forks_session_content(
    const content::ContentValidationReport& report) {
    auto environment = game::GameRuntimeEnvironment::initialize({}, report);
    assert(environment);
    auto session_runtime = environment.value().create_session_runtime();
    assert(session_runtime);
    assert(session_runtime.value().is_initialized());
    assert(session_runtime.value().startup_report().prototype_count ==
           environment.value().startup_report().prototype_count);
    assert(session_runtime.value().start_session(make_local_request(report, 7)));
    assert(session_runtime.value().shutdown());
    assert(environment.value().is_initialized());
    assert(environment.value().shutdown());
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
    auto duplicate_cleanup = first->register_cleanup("second", [] { return core::Status::ok(); });
    assert(!duplicate_cleanup &&
           duplicate_cleanup.error().code == "runtime_session.duplicate_cleanup");
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
    assert(teardown.server_entities_after == 0);
    assert(teardown.physics_bodies_after == 0);
    assert(teardown.session_jobs_after == 0);
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
    request.initial_runtime_time_ms = 100'000;
    assert(runtime.start_session(std::move(request)));
    assert(runtime.session()->server() == nullptr);
    assert(runtime.session()->client() != nullptr);
    assert(runtime.session()->connection_state() == game::SessionConnectionState::connecting);
    assert(runtime.run_frame({16'667, 100'016}));
    auto timed_out = runtime.run_frame({16'667, 105'001});
    assert(!timed_out);
    assert(timed_out.error().code == "transport_client.handshake_timeout");
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
    test_startup_phases_are_real_runtime_boundaries(report);
    test_startup_cancellation_is_cooperative(report);
    test_application_runtime_forks_session_content(report);
    test_local_teardown_and_replacement(report);
    test_remote_attempt_cancels_and_recovers(report);
    return 0;
}
