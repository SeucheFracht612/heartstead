#include "engine/content/content_validation.hpp"
#include "engine/world/chunks/chunk_replication.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/runtime/client_runtime.hpp"
#include "game/runtime/game_runtime.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace heartstead;

std::filesystem::path source_root() {
    return std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR);
}

game::GameRuntime make_runtime(const content::ContentValidationReport& report) {
    auto runtime = game::GameRuntime::initialize({}, report);
    assert(runtime);
    return std::move(runtime).value();
}

game::SessionRequest make_session_request(const content::ContentValidationReport& report) {
    auto metadata = content::save_metadata_from_content_report(
        report, "server-chunk-subscription-test", game::foundation::world_seed);
    assert(metadata);
    game::SessionRequest request;
    request.metadata = std::move(metadata).value();
    request.scenario_id = "base:scenarios/homestead";
    return request;
}

game::RuntimeFrameStats run_frame(game::GameRuntime& runtime, std::int64_t& now_ms) {
    now_ms += 17;
    auto frame = runtime.run_frame({16'667, now_ms});
    assert(frame);
    assert(frame.value().server_ticks.size() == 1);
    return std::move(frame).value();
}

const game::ServerChunkSubscriptionClientSnapshot&
only_subscription_client(const game::ServerRuntime& server,
                         std::vector<game::ServerChunkSubscriptionClientSnapshot>& storage) {
    storage = server.chunk_subscription_clients();
    assert(storage.size() == 1);
    return storage.front();
}

world::VoxelCell foundation_surface_cell(const game::ServerRuntime& server) {
    const auto* chunk = server.world().chunks().find({0, 0, 0});
    assert(chunk != nullptr);
    auto cell = chunk->get({8, 0, 8});
    assert(cell && !cell.value().is_air());
    return cell.value();
}

void set_player_position(game::ServerRuntime& server, core::NetId client_id,
                         world::WorldPosition position) {
    auto* player = server.player_for_client(client_id);
    assert(player != nullptr);
    player->state.position = position;
    player->state.fall_origin = position;
    player->state.scripted_start = position;
    player->state.scripted_target = position;
}

void receive_and_synchronize(game::ServerRuntime& server, core::NetId client_id,
                             game::ClientRuntime& client, std::uint64_t tick) {
    auto messages = server.drain_client_messages(client_id);
    assert(messages);
    assert(client.receive(messages.value()));
    auto synchronized = client.synchronize(tick, std::numeric_limits<std::size_t>::max());
    assert(synchronized);
}

void test_runtime_interest_is_bounded_and_ignores_unsubscribed_chunks(
    const content::ContentValidationReport& report) {
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.fixed_step = {60, 4, 250'000};
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    auto& server = *session->server();

    std::vector<game::ServerChunkSubscriptionClientSnapshot> clients;
    const auto& client = only_subscription_client(server, clients);
    assert(client.initial_state_published);
    assert(client.converged);
    assert(client.center == (world::ChunkCoord{0, 0, 0}));
    assert(client.subscriptions.size() == config.chunk_subscriptions.desired_chunk_count());
    assert(client.subscriptions.size() <= config.chunk_subscriptions.max_chunks_per_client);
    assert(std::ranges::is_sorted(client.subscriptions));
    assert(std::ranges::adjacent_find(client.subscriptions) == client.subscriptions.end());

    constexpr world::ChunkCoord unrelated{500, 0, 500};
    auto& far_chunk = server.world().chunks().get_or_create(unrelated);
    assert(far_chunk.set({0, 0, 0}, foundation_surface_cell(server)));
    assert(!std::ranges::binary_search(client.subscriptions, unrelated));

    std::int64_t now_ms = 0;
    auto frame = run_frame(runtime, now_ms);
    const auto& stats = frame.server_ticks.front().chunk_subscriptions;
    assert(stats.client_count == 1);
    assert(stats.maximum_client_subscription_count <=
           config.chunk_subscriptions.max_chunks_per_client);
    assert(!session->client()->world().chunks().contains(unrelated));
    clients = server.chunk_subscription_clients();
    assert(clients.size() == 1);
    assert(!std::ranges::binary_search(clients.front().subscriptions, unrelated));
}

void test_teleport_transitions_and_client_removals_are_bounded(
    const content::ContentValidationReport& report) {
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.fixed_step = {60, 4, 250'000};
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    auto& server = *session->server();
    const auto client_id = session->client()->client_id();

    std::vector<world::ChunkCoord> initially_resident;
    for (const auto* chunk : session->client()->world().chunks().records()) {
        initially_resident.push_back(chunk->coord());
    }
    assert(!initially_resident.empty());

    auto teleported = world::WorldPosition::from_anchor({3'200, 1, 0}, {0.5, 0.0, 0.5});
    assert(teleported);
    set_player_position(server, client_id, teleported.value());
    constexpr world::ChunkCoord expected_center{100, 0, 0};

    std::int64_t now_ms = 0;
    std::uint32_t removal_message_count = 0;
    bool converged = false;
    for (std::uint32_t update = 0; update < 24; ++update) {
        auto frame = run_frame(runtime, now_ms);
        const auto& stats = frame.server_ticks.front().chunk_subscriptions;
        assert(stats.added_subscription_count <=
               config.chunk_subscriptions.max_additions_per_update);
        assert(stats.removed_subscription_count <=
               config.chunk_subscriptions.max_removals_per_update);
        assert(stats.maximum_client_subscription_count <=
               config.chunk_subscriptions.max_chunks_per_client);
        removal_message_count += stats.removal_message_count;

        auto clients = server.chunk_subscription_clients();
        assert(clients.size() == 1);
        assert(clients.front().center == expected_center);
        assert(clients.front().subscriptions.size() <=
               config.chunk_subscriptions.max_chunks_per_client);
        if (clients.front().converged && clients.front().subscriptions.size() ==
                                             config.chunk_subscriptions.desired_chunk_count()) {
            converged = true;
            break;
        }
    }
    assert(converged);
    assert(removal_message_count >= initially_resident.size());
    for (const auto coordinate : initially_resident) {
        assert(!session->client()->world().chunks().contains(coordinate));
    }
}

void test_shared_chunk_encoding_is_reused_across_clients(
    const content::ContentValidationReport& report) {
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.fixed_step = {60, 4, 250'000};
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    auto& server = *session->server();

    auto connected = server.connect_client();
    assert(connected);
    world::WorldStateDesc client_world;
    client_world.metadata = server.world().metadata();
    client_world.voxel_palette = server.world().voxel_palette_manifest();
    game::ClientRuntime secondary(connected.value(), std::move(client_world),
                                  &server.replication_registry(), &server.voxel_palette());
    receive_and_synchronize(server, connected.value(), secondary, 0);
    assert(secondary.is_connected());
    assert(secondary.local_player_snapshot() != nullptr);

    auto* chunk = server.world().chunks().find({0, 0, 0});
    assert(chunk != nullptr);
    constexpr world::VoxelCoord edited{31, 31, 31};
    auto previous = chunk->get(edited);
    assert(previous);
    const auto replacement =
        previous.value().is_air() ? foundation_surface_cell(server) : world::VoxelCell::air();
    assert(chunk->set(edited, replacement));

    std::int64_t now_ms = 0;
    auto frame = run_frame(runtime, now_ms);
    const auto& stats = frame.server_ticks.front().chunk_subscriptions;
    assert(stats.client_count == 2);
    assert(stats.snapshot_serialization_operation_count > 0);
    assert(stats.snapshot_chunk_count == stats.snapshot_serialization_operation_count * 2U);
    assert(stats.snapshot_slice_message_count ==
           stats.snapshot_chunk_count * world::VoxelChunk::edge_length);
    assert(stats.partial_snapshot_count == 0);
    receive_and_synchronize(server, connected.value(), secondary, 1);

    const auto* primary_chunk = session->client()->world().chunks().find({0, 0, 0});
    const auto* secondary_chunk = secondary.world().chunks().find({0, 0, 0});
    assert(primary_chunk != nullptr && secondary_chunk != nullptr);
    const auto primary_cell = primary_chunk->get(edited);
    const auto secondary_cell = secondary_chunk->get(edited);
    assert(primary_cell && secondary_cell);
    assert(primary_cell.value().type == replacement.type);
    assert(secondary_cell.value().type == replacement.type);
}

void test_reliable_backlog_defers_and_recovers_without_partial_publication(
    const content::ContentValidationReport& report) {
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.fixed_step = {60, 4, 250'000};
    config.max_pending_reliable_messages = 256;
    config.max_pending_reliable_messages_per_client = 128;
    config.max_reliable_delivery_messages_per_tick = 1;
    config.max_reliable_delivery_messages_per_client_per_tick = 1;
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    auto& server = *session->server();
    const auto client_id = session->client()->client_id();

    std::vector<net::TransportMessage> backlog;
    constexpr std::size_t backlog_count = 112;
    backlog.reserve(backlog_count);
    for (std::size_t index = 0; index < backlog_count; ++index) {
        backlog.push_back({net::TransportMessageKind::replication, net::TransportChannel::reliable,
                           9'000'000U + index, "test.chunk_backlog", std::to_string(index), 0});
    }
    assert(server.host().send_reliable_replication_batch(client_id, std::move(backlog)));
    assert(server.host().pending_outbound_message_count(client_id) == backlog_count);

    auto* chunk = server.world().chunks().find({0, 0, 0});
    assert(chunk != nullptr);
    auto previous = chunk->get({30, 30, 30});
    assert(previous);
    const auto replacement =
        previous.value().is_air() ? foundation_surface_cell(server) : world::VoxelCell::air();
    assert(chunk->set({30, 30, 30}, replacement));

    std::int64_t now_ms = 0;
    auto first = run_frame(runtime, now_ms);
    const auto& first_stats = first.server_ticks.front().chunk_subscriptions;
    assert(first_stats.snapshot_chunk_count == 0);
    assert(first_stats.snapshot_serialization_operation_count == 0);
    assert(first_stats.deferred_snapshot_count > 0);
    assert(first_stats.reliable_admission_deferral_count > 0);
    assert(first_stats.partial_snapshot_count == 0);

    bool admitted_after_pressure = false;
    for (std::uint32_t tick = 0; tick < 40; ++tick) {
        auto frame = run_frame(runtime, now_ms);
        const auto& stats = frame.server_ticks.front().chunk_subscriptions;
        assert(stats.partial_snapshot_count == 0);
        assert(server.host().pending_outbound_message_count(client_id) <=
               config.max_pending_reliable_messages_per_client);
        if (stats.snapshot_chunk_count > 0) {
            admitted_after_pressure = true;
            break;
        }
    }
    assert(admitted_after_pressure);
    assert(session->client()->is_connected());
}

void test_collision_chunks_precede_deferred_initial_state(
    const content::ContentValidationReport& report) {
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.fixed_step = {60, 4, 250'000};
    config.max_pending_reliable_messages = 64;
    config.max_pending_reliable_messages_per_client = 64;
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    assert(session->client()->is_connected());
    assert(session->client()->world().chunks().contains({0, 0, 0}));

    auto clients = session->server()->chunk_subscription_clients();
    assert(clients.size() == 1);
    assert(!clients.front().initial_state_published);
    assert(session->client()->local_player_snapshot() == nullptr);

    std::int64_t now_ms = 0;
    bool published = false;
    for (std::uint32_t tick = 0; tick < 30; ++tick) {
        auto frame = run_frame(runtime, now_ms);
        clients = session->server()->chunk_subscription_clients();
        assert(clients.size() == 1);
        if (clients.front().initial_state_published) {
            published = true;
            assert(frame.server_ticks.front().chunk_subscriptions.published_initial_state_count ==
                   1);
            break;
        }
    }
    assert(published);
    assert(session->client()->local_player_snapshot() != nullptr);
    movement::PlayerInputFrame input;
    input.tick = 1;
    input.sequence = 1;
    input.move_z = 32'767;
    assert(session->submit_player_input(input, now_ms));
}

} // namespace

int main() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    test_runtime_interest_is_bounded_and_ignores_unsubscribed_chunks(report);
    test_teleport_transitions_and_client_removals_are_bounded(report);
    test_shared_chunk_encoding_is_reused_across_clients(report);
    test_reliable_backlog_defers_and_recovers_without_partial_publication(report);
    test_collision_chunks_precede_deferred_initial_state(report);
    return 0;
}
