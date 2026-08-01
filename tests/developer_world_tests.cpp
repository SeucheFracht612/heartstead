#include "engine/content/content_validation.hpp"
#include "engine/scenarios/scenario_fixture.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/runtime/game_runtime.hpp"
#include "game/scenarios/developer_world_registry.hpp"
#include "game/scenarios/scenario_setup.hpp"

#include <cassert>
#include <filesystem>
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

    auto runtime = runtime_for(report);
    assert(runtime.start_session(std::move(request).value()));
    const auto& world = runtime.session()->server()->world();
    const auto* fixture = world.mod_states().find("engine", "scenario.fixture");
    assert(fixture != nullptr && fixture->encoded_state == "renderer_proof");
    assert(world.chunks().find(scenarios::renderer_proof_center) != nullptr);
    const auto* player =
        runtime.session()->server()->player_for_client(runtime.session()->client()->client_id());
    assert(player != nullptr);
    assert(player->state.position.anchor.x > 30'000'000'000LL);
    assert(player->state.position.anchor.z < -30'000'000'000LL);
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
