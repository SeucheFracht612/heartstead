#include "game/presentation/presentation_world.hpp"

#include <cassert>

using namespace heartstead;

namespace {

game::PresentationObjectUpdate make_update(core::NetId source, std::uint64_t revision,
                                           world::WorldPosition position) {
    game::PresentationObjectUpdate update;
    update.source_net_id = source;
    update.visual_prototype = *core::PrototypeId::parse("test:visual/player");
    update.transform.position = position;
    update.local_bounds = {{-0.5F, 0.0F, -0.5F}, {0.5F, 2.0F, 0.5F}};
    update.source_revision = revision;
    return update;
}

void test_retained_presentation_and_immutable_snapshot_extraction() {
    constexpr std::int64_t coordinate = 8'000'000'000'000'000LL;
    auto first_position =
        world::WorldPosition::from_anchor({coordinate, 4, -coordinate}, {0.25, 0.5, 0.75});
    assert(first_position);
    game::PresentationWorld presentation;
    const auto source = core::NetId::from_value(77);
    auto inserted = presentation.upsert_object(make_update(source, 1, first_position.value()));
    assert(inserted);
    assert(presentation.stats().retained_object_count == 1);
    auto snapshot = presentation.extract(10);
    assert(snapshot.simulation_tick == 10);
    assert(snapshot.objects.size() == 1);
    assert(snapshot.objects.front().current_transform.position == first_position.value());
    assert(snapshot.objects.front().previous_transform.position == first_position.value());
    assert(snapshot.objects.front().current_locomotion.kind ==
           animation::LocomotionAnimationKind::idle);

    auto second_position =
        world::WorldPosition::from_anchor({coordinate, 4, -coordinate}, {0.25, 0.5, 1.75});
    assert(second_position);
    auto second_update = make_update(source, 2, second_position.value());
    second_update.locomotion.kind = animation::LocomotionAnimationKind::walk;
    second_update.locomotion.transition_from = animation::LocomotionAnimationKind::idle;
    second_update.locomotion.transition_tick = 1;
    second_update.locomotion.phase = 16'384;
    auto updated = presentation.upsert_object(second_update);
    assert(updated && updated.value() == inserted.value());
    snapshot = presentation.extract(11);
    assert(snapshot.objects.front().previous_transform.position == first_position.value());
    assert(snapshot.objects.front().current_transform.position == second_position.value());
    assert(snapshot.objects.front().source_revision == 2);
    assert(snapshot.objects.front().previous_locomotion.kind ==
           animation::LocomotionAnimationKind::idle);
    assert(snapshot.objects.front().current_locomotion.kind ==
           animation::LocomotionAnimationKind::walk);
    second_update.visual_states = {{"powered", "true"}, {"fill_level", "full"}};
    second_update.source_revision = 3;
    assert(presentation.upsert_object(second_update));
    snapshot = presentation.extract(12);
    assert(snapshot.objects.front().visual_states == second_update.visual_states);
    const auto unchanged_revision = snapshot.presentation_revision;
    assert(presentation.upsert_object(second_update));
    assert(presentation.extract(12).presentation_revision == unchanged_revision);
    assert(!presentation.upsert_object(make_update(source, 2, first_position.value())));

    assert(presentation.remove_object(source));
    assert(presentation.find_object(source) == nullptr);
    auto replacement = presentation.upsert_object(make_update(source, 3, second_position.value()));
    assert(replacement);
    assert(replacement.value().index() == inserted.value().index());
    assert(replacement.value().generation() != inserted.value().generation());
}

void test_presentation_validation_rejects_unsafe_render_data() {
    game::PresentationWorld presentation;
    auto invalid = make_update({}, 1, world::WorldPosition{});
    assert(!presentation.upsert_object(invalid));
    invalid = make_update(core::NetId::from_value(1), 0, world::WorldPosition{});
    assert(!presentation.upsert_object(invalid));
    invalid = make_update(core::NetId::from_value(1), 1, world::WorldPosition{});
    invalid.visual_states = {{"powered", "true"}, {"powered", "false"}};
    assert(!presentation.upsert_object(invalid));
}

} // namespace

int main() {
    test_retained_presentation_and_immutable_snapshot_extraction();
    test_presentation_validation_rejects_unsafe_render_data();
    return 0;
}
