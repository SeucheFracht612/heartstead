#include "engine/content/content_validation.hpp"
#include "engine/scenarios/scenario_fixture.hpp"
#include "engine/world/worldgen/terrain_generator.hpp"
#include "game/features/animals/wandering_animal_module.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/runtime/game_runtime.hpp"
#include "game/scenarios/developer_world_registry.hpp"
#include "game/scenarios/scenario_setup.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {

using namespace heartstead;

save::SaveMetadata metadata_for(const content::ContentValidationReport& report) {
    auto metadata = content::save_metadata_from_content_report(report, "developer-world-test",
                                                               game::foundation::world_seed);
    assert(metadata);
    return std::move(metadata).value();
}

game::GameRuntime runtime_for(const content::ContentValidationReport& report) {
    auto runtime = game::GameRuntime::initialize({}, report);
    assert(runtime);
    return std::move(runtime).value();
}

void test_discovery_and_browser_data(const content::ContentValidationReport& report) {
    auto registry = game::DeveloperWorldRegistry::create(report.scenario_definitions);
    assert(registry);
    assert(registry.value().entries().size() == 2);
    const auto* foundation = registry.value().find("base:scenarios/foundation_slice");
    const auto* renderer = registry.value().find("base:scenarios/renderer_proof");
    assert(foundation != nullptr && renderer != nullptr);
    assert(foundation->category == scenarios::ScenarioCategory::gameplay);
    assert(foundation->persistence == scenarios::ScenarioPersistencePolicy::ephemeral);
    assert(renderer->category == scenarios::ScenarioCategory::rendering);
    assert(renderer->world_source == scenarios::ScenarioWorldSource::packaged_fixture);
    assert(renderer->persistence == scenarios::ScenarioPersistencePolicy::temporary_copy);
    assert(!foundation->description.empty() && !renderer->description.empty());
    assert(registry.value().filter(scenarios::ScenarioCategory::rendering).size() == 1);
    assert(registry.value().filter(std::nullopt, "floating-origin").size() == 1);
    assert(registry.value().filter(std::nullopt, "expansive").size() == 1);
    assert(registry.value().filter(std::nullopt, "renderer proof").size() == 1);
    assert(game::default_scenario_setup_registry().contains("renderer_proof"));
}

void test_generated_developer_world_launch(const content::ContentValidationReport& report) {
    auto registry = game::DeveloperWorldRegistry::create(report.scenario_definitions);
    assert(registry);
    auto request = registry.value().make_launch_request("base:scenarios/foundation_slice",
                                                        metadata_for(report), true);
    assert(request);
    assert(request.value().world_source == game::WorldSourceKind::developer_scenario);
    assert(request.value().persistence == game::PersistencePolicy::ephemeral);
    assert(!request.value().save_path.has_value());

    auto runtime = runtime_for(report);
    assert(runtime.start_session(std::move(request).value()));
    const auto* scenario =
        runtime.session()->server()->world().mod_states().find("engine", "scenario.id");
    assert(scenario != nullptr && scenario->encoded_state == "base:scenarios/foundation_slice");
    auto snapshot = runtime.capture_save_snapshot();
    assert(snapshot);
    const auto generator_version = std::ranges::find_if(
        snapshot.value().mod_states, [](const save::ModStateSaveRecord& state) {
            return state.mod_id == "engine" && state.state_key == "world.generator_version";
        });
    assert(generator_version != snapshot.value().mod_states.end());
    assert(generator_version->encoded_state ==
           std::to_string(world::deterministic_terrain_generator_version));
    assert(runtime.shutdown());
}

void test_packaged_renderer_fixture_launch(const content::ContentValidationReport& report) {
    auto registry = game::DeveloperWorldRegistry::create(report.scenario_definitions);
    assert(registry);
    auto request = registry.value().make_launch_request("base:scenarios/renderer_proof",
                                                        metadata_for(report), true);
    assert(request);
    assert(request.value().world_source == game::WorldSourceKind::packaged_fixture);
    assert(request.value().persistence == game::PersistencePolicy::temporary_copy);
    request.value().runtime.chunk_streaming.temporal_retention_ms = 1;
    request.value().runtime.chunk_streaming.max_evictions_per_update = 3;
    request.value().runtime.gameplay_modules.push_back(
        std::make_shared<game::animals::WanderingAnimalModule>());

    auto runtime = runtime_for(report);
    assert(runtime.start_session(std::move(request).value()));
    const auto& world = runtime.session()->server()->world();
    const auto* fixture = world.mod_states().find("engine", "scenario.fixture");
    assert(fixture != nullptr && fixture->encoded_state == "renderer_proof");
    assert(world.chunks().find(scenarios::renderer_proof_center) != nullptr);
    const auto initial_server_chunk_count = world.chunks().chunk_count();
    const auto initial_server_chunk_identities = world.chunks().identities();
    const auto initial_client_chunk_count =
        runtime.session()->client()->world().chunks().chunk_count();
    assert(initial_server_chunk_count == 9);
    assert(initial_client_chunk_count == initial_server_chunk_count);
    auto* player =
        runtime.session()->server()->player_for_client(runtime.session()->client()->client_id());
    assert(player != nullptr);
    assert(player->state.position.anchor.x > 30'000'000'000LL);
    assert(player->state.position.anchor.z < -30'000'000'000LL);

    const auto original_anchor = player->state.position.anchor;
    auto first_frame = runtime.run_frame({16'667, 17});
    assert(first_frame && !first_frame.value().server_ticks.empty());
    assert(first_frame.value().server_ticks.back().chunk_loading.submitted_requests > 0);

    player->state.position.anchor.x +=
        world::VoxelChunk::edge_length * static_cast<std::int64_t>(40);
    auto teleport_frame = runtime.run_frame({16'667, 34});
    assert(teleport_frame && !teleport_frame.value().server_ticks.empty());
    const auto teleport_streaming = teleport_frame.value().server_ticks.back().chunk_streaming;
    assert(teleport_streaming.enabled);
    assert(teleport_streaming.teleport_mode);
    assert(teleport_streaming.evicted_chunk_count == 3);
    assert(teleport_streaming.deferred_eviction_count > 0);
    player->state.position.anchor = original_anchor;

    world::ChunkLoadSchedulerStats loading_stats;
    game::ServerChunkStreamingTickStats streaming_stats;
    for (std::int64_t frame_index = 3; frame_index <= 302; ++frame_index) {
        auto frame = runtime.run_frame({16'667, frame_index * 17});
        assert(frame);
        if (!frame.value().server_ticks.empty()) {
            loading_stats = frame.value().server_ticks.back().chunk_loading;
            streaming_stats = frame.value().server_ticks.back().chunk_streaming;
        }
    }
    std::size_t expected_chunk_count = 0;
    for (std::int64_t z = -scenarios::renderer_proof_stream_radius_chunks;
         z <= scenarios::renderer_proof_stream_radius_chunks; ++z) {
        for (std::int64_t x = -scenarios::renderer_proof_stream_radius_chunks;
             x <= scenarios::renderer_proof_stream_radius_chunks; ++x) {
            expected_chunk_count +=
                x * x + z * z <= scenarios::renderer_proof_stream_radius_chunks *
                                     scenarios::renderer_proof_stream_radius_chunks;
        }
    }
    assert(world.chunks().chunk_count() >= expected_chunk_count);
    assert(world.chunks().chunk_count() <= streaming_stats.target_resident_chunk_count);
    assert(runtime.session()->client()->world().chunks().chunk_count() ==
           world.chunks().chunk_count());
    assert(streaming_stats.enabled);
    assert(streaming_stats.desired_chunk_count == expected_chunk_count);
    assert(streaming_stats.pending_load_count == 0);
    assert(streaming_stats.deferred_eviction_count == 0);
    assert(streaming_stats.projected_resident_overage == 0);
    assert(streaming_stats.unresolved_resident_overage == 0);
    assert(streaming_stats.lifetime.planning_updates > 0);
    assert(streaming_stats.lifetime.teleport_updates >= 2);
    assert(streaming_stats.lifetime.speculative_submissions > 0);
    assert(streaming_stats.lifetime.speculative_publications > 0);
    assert(streaming_stats.lifetime.active_speculative_requests > 0);
    assert(streaming_stats.lifetime.active_speculative_requests <= 16);
    for (const auto* chunk : world.chunks().records()) {
        assert(runtime.session()->client()->local_chunk_snapshot_is_current(*chunk));
    }
    assert(std::ranges::any_of(initial_server_chunk_identities, [&world](const auto identity) {
        const auto* reloaded = world.chunks().find(identity.coordinate);
        return reloaded != nullptr && reloaded->identity() != identity;
    }));
    for (std::int64_t z = -scenarios::renderer_proof_stream_radius_chunks;
         z <= scenarios::renderer_proof_stream_radius_chunks; ++z) {
        for (std::int64_t x = -scenarios::renderer_proof_stream_radius_chunks;
             x <= scenarios::renderer_proof_stream_radius_chunks; ++x) {
            if (x * x + z * z <= scenarios::renderer_proof_stream_radius_chunks *
                                     scenarios::renderer_proof_stream_radius_chunks) {
                assert(world.chunks().contains({scenarios::renderer_proof_center.x + x,
                                                scenarios::renderer_proof_center.y,
                                                scenarios::renderer_proof_center.z + z}));
            }
        }
    }
    assert(loading_stats.in_flight_requests == 0);
    assert(loading_stats.reserved_working_bytes == 0);
    assert(loading_stats.reserved_working_bytes_high_water > 0);
    assert(loading_stats.reserved_working_bytes_high_water <=
           world::ChunkLoadSchedulerConfig{}.max_reserved_working_bytes);
    assert(loading_stats.published_requests > expected_chunk_count - initial_server_chunk_count);
    assert(loading_stats.cancelled_requests > 0);
    assert(loading_stats.last_worker_ms > 0.0);
    assert(loading_stats.maximum_pipeline_latency_ms > 0.0);
    assert(loading_stats.maximum_publication_time_us > 0);
    assert(std::ranges::any_of(world.chunks().records(), [](const auto* chunk) {
        const auto coord = chunk->coord();
        return coord.x < scenarios::renderer_proof_center.x - 1 ||
               coord.x > scenarios::renderer_proof_center.x + 1 ||
               coord.z < scenarios::renderer_proof_center.z - 1 ||
               coord.z > scenarios::renderer_proof_center.z + 1;
    }));
    auto render_snapshot = runtime.capture_render_snapshot();
    assert(render_snapshot);
    assert(std::ranges::all_of(render_snapshot.value().objects, [player](const auto& object) {
        return static_cast<bool>(
            world::to_camera_relative(object.current_transform.position,
                                      world::FloatingOrigin{player->state.position.anchor}));
    }));
    assert(runtime.shutdown());
    assert(runtime.last_teardown_report()->presentation_objects_after == 0);
}

} // namespace

int main() {
    const auto report =
        content::ContentValidation::validate(std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR));
    assert(!report.has_errors());
    test_discovery_and_browser_data(report);
    test_generated_developer_world_launch(report);
    test_packaged_renderer_fixture_launch(report);
    return 0;
}
