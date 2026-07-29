#include "game/features/animals/wandering_animal_module.hpp"
#include "game/testing/headless_session.hpp"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <memory>
#include <ranges>
#include <vector>

using namespace heartstead;

namespace {

std::filesystem::path source_root() {
    return std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR);
}

void test_local_headless_session_advances_shared_runtime() {
    game::HeadlessSessionDesc desc;
    desc.source_root = source_root();
    auto harness = game::HeadlessSessionHarness::create(std::move(desc));
    assert(harness);
    auto report = harness.value()->run_ticks(5);
    assert(report);
    assert(report.value().requested_tick_count == 5);
    assert(report.value().completed_tick_count == 5);
    assert(report.value().last_frame.authoritative_world_tick == 1);
    assert(harness.value()->runtime().session()->client() != nullptr);
    auto snapshot = harness.value()->runtime().capture_render_snapshot();
    assert(snapshot && snapshot.value().objects.size() == 2);
    assert(std::ranges::any_of(snapshot.value().objects, [](const auto& object) {
        return object.visual_prototype ==
               *core::PrototypeId::parse("base:entities/foundation_material_showcase");
    }));
    assert(harness.value()->shutdown());
}

void test_dedicated_headless_session_uses_same_harness_without_client_services() {
    game::HeadlessSessionDesc desc;
    desc.source_root = source_root();
    desc.runtime.create_client = false;
    auto harness = game::HeadlessSessionHarness::create(std::move(desc));
    assert(harness);
    auto report = harness.value()->run_ticks(3);
    assert(report && report.value().completed_tick_count == 3);
    assert(harness.value()->runtime().session()->server() != nullptr);
    assert(harness.value()->runtime().session()->client() == nullptr);
    assert(!harness.value()->runtime().capture_render_snapshot());
    assert(harness.value()->shutdown());
}

struct AnimalFrame {
    world::WorldTransform transform;
    animation::ReplicatedLocomotionAnimation locomotion;

    friend bool operator==(const AnimalFrame&, const AnimalFrame&) = default;
};

std::vector<AnimalFrame> run_wandering_animal_trajectory() {
    game::HeadlessSessionDesc desc;
    desc.source_root = source_root();
    game::animals::WanderingAnimalConfig animal_config;
    animal_config.seed = 1;
    animal_config.segment_ticks = 30;
    desc.runtime.gameplay_modules.push_back(
        std::make_shared<game::animals::WanderingAnimalModule>(animal_config));
    auto harness = game::HeadlessSessionHarness::create(std::move(desc));
    assert(harness);
    const auto animal_prototype = *core::PrototypeId::parse("base:entities/test_animal");
    std::vector<AnimalFrame> trajectory;
    trajectory.reserve(70);
    for (std::uint32_t tick = 0; tick < 70; ++tick) {
        auto report = harness.value()->run_ticks(1);
        assert(report);
        assert(report.value().last_frame.server_ticks.back().entity_motion_snapshot_count == 1);
        assert(report.value().last_frame.client.entity_motion_snapshot_count == 1);
        auto snapshot = harness.value()->runtime().capture_render_snapshot();
        assert(snapshot);
        const auto found = std::ranges::find(snapshot.value().objects, animal_prototype,
                                             &game::RenderObjectSnapshot::visual_prototype);
        assert(found != snapshot.value().objects.end());
        trajectory.push_back({found->current_transform, found->current_locomotion});
    }
    assert(harness.value()->shutdown());
    return trajectory;
}

void test_wandering_animal_is_replicated_and_deterministic() {
    const auto first = run_wandering_animal_trajectory();
    const auto second = run_wandering_animal_trajectory();
    assert(first == second);
    assert(first.front().transform.position != first[29].transform.position);
    assert(std::ranges::any_of(first, [](const AnimalFrame& frame) {
        return frame.locomotion.kind == animation::LocomotionAnimationKind::walk;
    }));
    assert(std::ranges::any_of(first, [](const AnimalFrame& frame) {
        return frame.locomotion.kind == animation::LocomotionAnimationKind::idle &&
               frame.locomotion.transition_from == animation::LocomotionAnimationKind::walk;
    }));
}

void test_foundation_scene_objects_resolve_visual_definitions() {
    const auto content = content::ContentValidation::validate(source_root());
    assert(!content.has_errors());

    game::HeadlessSessionDesc desc;
    desc.source_root = source_root();
    desc.runtime.gameplay_modules.push_back(
        std::make_shared<game::animals::WanderingAnimalModule>());
    auto harness = game::HeadlessSessionHarness::create(std::move(desc));
    assert(harness);
    auto report = harness.value()->run_ticks(5);
    assert(report);
    auto snapshot = harness.value()->runtime().capture_render_snapshot();
    assert(snapshot);
    assert(snapshot.value().objects.size() == 3);
    for (const auto& object : snapshot.value().objects) {
        const auto* visual = content.visual_definitions.find_for_entity(object.visual_prototype);
        assert(visual != nullptr);
        assert(content.asset_catalog.find_active(visual->model_asset) != nullptr);
    }
    assert(harness.value()->shutdown());
}

} // namespace

int main() {
    test_local_headless_session_advances_shared_runtime();
    test_dedicated_headless_session_uses_same_harness_without_client_services();
    test_wandering_animal_is_replicated_and_deterministic();
    test_foundation_scene_objects_resolve_visual_definitions();
    return 0;
}
