#include "engine/simulation/fixed_step.hpp"
#include "engine/simulation/simulation_lod.hpp"
#include "engine/simulation/simulation_scheduler.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

using namespace heartstead;

namespace {

simulation::SimulationSubject make_lod_subject(std::uint64_t save_id, simulation::SimulationLod lod,
                                               simulation::WorldTick last_update_time_ms,
                                               std::uint32_t work_units = 1) {
    simulation::SimulationSubject subject;
    subject.save_id = core::SaveId::from_value(save_id);
    subject.kind = simulation::SimulationSubjectKind::entity;
    subject.last_update_time_ms = last_update_time_ms;
    subject.estimated_update_work_units = work_units;
    subject.forced_lod = lod;
    return subject;
}

void test_fixed_step_clock_is_bounded_and_deterministic() {
    simulation::FixedStepClock clock({60, 4, 250'000});

    auto first = clock.advance(16'666);
    assert(first);
    assert(first.value().step_count == 0);
    assert(first.value().interpolation_alpha > 0.99);

    auto second = clock.advance(1);
    assert(second);
    assert(second.value().step_count == 1);
    assert(second.value().first_tick == 1);
    assert(clock.tick() == 1);
    assert(second.value().interpolation_alpha < 0.001);

    auto stalled = clock.advance(1'000'000);
    assert(stalled);
    assert(stalled.value().step_count == 4);
    assert(stalled.value().dropped_time_us >= 750'000);
    assert(stalled.value().interpolation_alpha >= 0.0);
    assert(stalled.value().interpolation_alpha < 1.0);
}

void test_tick_events_have_explicit_lifetime() {
    simulation::TickEvents events;
    assert(events.begin_tick(7));
    assert(!events.begin_tick(8));

    simulation::EntitySpawned spawned;
    spawned.entity = entities::EntityId::from_parts(42, 3);
    spawned.prototype = *core::PrototypeId::parse("base:entities/player");
    assert(events.entity_spawned.append(spawned));
    assert(events.event_count() == 1);
    assert(events.seal());
    assert(!events.entity_spawned.append(spawned));
    assert(events.entity_spawned.events().front().entity == spawned.entity);

    events.clear();
    assert(events.event_count() == 0);
    assert(!events.is_active());
    assert(events.begin_tick(8));
    assert(events.tick() == 8);
}

void test_scheduler_orders_phases_and_dependencies() {
    simulation::SimulationScheduler scheduler;
    std::vector<std::string> order;

    assert(scheduler.register_system({
        "gameplay.second",
        simulation::SimulationPhase::gameplay,
        {"gameplay.first"},
        [&order](simulation::SimulationContext&) {
            order.emplace_back("gameplay.second");
            return core::Status::ok();
        },
    }));
    assert(scheduler.register_system({
        "commands",
        simulation::SimulationPhase::commands,
        {},
        [&order](simulation::SimulationContext&) {
            order.emplace_back("commands");
            return core::Status::ok();
        },
    }));
    assert(scheduler.register_system({
        "gameplay.first",
        simulation::SimulationPhase::gameplay,
        {"commands"},
        [&order](simulation::SimulationContext& context) {
            order.emplace_back("gameplay.first");
            simulation::InventoryChanged changed;
            changed.inventory = core::SaveId::from_value(9);
            changed.revision = context.tick;
            return context.events->inventory_changed.append(changed);
        },
    }));
    assert(scheduler.finalize());

    const auto names = scheduler.ordered_system_names();
    assert(
        (names == std::vector<std::string_view>{"commands", "gameplay.first", "gameplay.second"}));

    simulation::TickEvents events;
    auto tick = scheduler.run_tick({11, 1.0 / 60.0, nullptr, nullptr, &events});
    assert(tick);
    assert((order == std::vector<std::string>{"commands", "gameplay.first", "gameplay.second"}));
    assert(tick.value().tick == 11);
    assert(tick.value().system_count == 3);
    assert(tick.value().event_count == 1);
    assert(events.is_sealed());
    assert(scheduler.timings().size() == 3);
    assert(scheduler.timings()[0].invocation_count == 1);
}

void test_scheduler_rejects_invalid_graphs_and_reports_failures() {
    simulation::SimulationScheduler cycle;
    assert(cycle.register_system(
        {"a", simulation::SimulationPhase::gameplay, {"b"}, [](simulation::SimulationContext&) {
             return core::Status::ok();
         }}));
    assert(cycle.register_system(
        {"b", simulation::SimulationPhase::gameplay, {"a"}, [](simulation::SimulationContext&) {
             return core::Status::ok();
         }}));
    assert(!cycle.finalize());

    simulation::SimulationScheduler scheduler;
    assert(scheduler.register_system(
        {"failure", simulation::SimulationPhase::gameplay, {}, [](simulation::SimulationContext&) {
             return core::Status::failure("test.failure", "intentional failure");
         }}));
    assert(scheduler.finalize());
    simulation::TickEvents events;
    auto result = scheduler.run_tick({1, 0.05, nullptr, nullptr, &events});
    assert(!result);
    assert(result.error().code == "simulation_scheduler.system_failed");
    assert(events.is_sealed());
}

void test_temporal_lod_budget_is_deadline_ordered_and_hard_bounded() {
    simulation::SimulationLodPolicy policy;
    policy.full_tick_interval_ms = 10;
    policy.simplified_tick_interval_ms = 100;
    policy.sleeping_tick_interval_ms = 1000;

    simulation::SimulationTickBudget budget;
    budget.maximum_updates_per_tick = 2;
    budget.maximum_work_units_per_tick = 3;
    budget.maximum_full_catch_up_delta_ms_per_update = 0;
    budget.maximum_simplified_catch_up_delta_ms_per_update = 200;
    budget.maximum_sleeping_catch_up_delta_ms_per_update = 500;
    budget.maximum_catch_up_delta_ms_per_tick = 500;

    std::vector<simulation::SimulationSubject> subjects;
    subjects.push_back(make_lod_subject(1, simulation::SimulationLod::full, 1900, 2));
    subjects.push_back(make_lod_subject(2, simulation::SimulationLod::simplified, 1000, 2));
    subjects.push_back(make_lod_subject(3, simulation::SimulationLod::sleeping, 0, 1));
    subjects.push_back(make_lod_subject(4, simulation::SimulationLod::simplified, 1900, 1));

    auto planned =
        simulation::SimulationLodPlanner::plan_budgeted_frame(subjects, {}, policy, budget, 2000);
    assert(planned);
    const auto& plan = planned.value();
    assert(plan.frame.due_tick_count == 4);
    assert(plan.updates.size() == 2);
    assert(plan.frame.decisions[plan.updates[0].decision_index].save_id ==
           core::SaveId::from_value(3));
    assert(plan.frame.decisions[plan.updates[1].decision_index].save_id ==
           core::SaveId::from_value(2));
    assert(plan.updates[0].update_delta_ms == 1500);
    assert(plan.updates[0].catch_up_delta_ms == 500);
    assert(plan.updates[0].remaining_delta_ms == 500);
    assert(plan.updates[0].next_update_time_ms == 1500);
    assert(plan.updates[1].update_delta_ms == 100);
    assert(plan.updates[1].catch_up_delta_ms == 0);
    assert(plan.updates[1].remaining_delta_ms == 900);
    assert(plan.updates[1].next_update_time_ms == 1100);
    assert(plan.deferred_due_count == 2);
    assert(plan.catch_up_due_count == 3);
    assert(plan.remaining_catch_up_count == 2);
    assert(plan.due_work_units == 6);
    assert(plan.scheduled_work_units == 3);
    assert(plan.deferred_work_units == 3);
    assert(plan.scheduled_delta_ms == 1600);
    assert(plan.deferred_delta_ms == 1100);
    assert(plan.scheduled_catch_up_delta_ms == 500);
    assert(plan.deferred_catch_up_delta_ms == 890);
    assert(plan.maximum_lateness_ms == 1000);
    assert(plan.budget_exhausted);
    assert(!plan.counters_saturated);
}

void test_temporal_lod_budget_has_stable_ties_and_preserves_fixed_full_steps() {
    simulation::SimulationLodPolicy policy;
    policy.full_tick_interval_ms = 10;
    policy.simplified_tick_interval_ms = 100;

    simulation::SimulationTickBudget budget;
    budget.maximum_updates_per_tick = 2;
    budget.maximum_work_units_per_tick = 2;
    budget.maximum_full_catch_up_delta_ms_per_update = 0;
    budget.maximum_simplified_catch_up_delta_ms_per_update = 100;
    budget.maximum_sleeping_catch_up_delta_ms_per_update = 100;
    budget.maximum_catch_up_delta_ms_per_tick = 200;

    const std::vector<simulation::SimulationSubject> tied{
        make_lod_subject(4, simulation::SimulationLod::simplified, 0),
        make_lod_subject(1, simulation::SimulationLod::simplified, 0),
        make_lod_subject(3, simulation::SimulationLod::simplified, 0),
        make_lod_subject(2, simulation::SimulationLod::simplified, 0),
    };
    auto planned =
        simulation::SimulationLodPlanner::plan_budgeted_frame(tied, {}, policy, budget, 200);
    assert(planned);
    assert(planned.value().frame.decisions[planned.value().updates[0].decision_index].save_id ==
           core::SaveId::from_value(1));
    assert(planned.value().frame.decisions[planned.value().updates[1].decision_index].save_id ==
           core::SaveId::from_value(2));

    budget.maximum_updates_per_tick = 1;
    budget.maximum_work_units_per_tick = 1;
    auto full = simulation::SimulationLodPlanner::plan_budgeted_frame(
        {make_lod_subject(8, simulation::SimulationLod::full, 0)}, {}, policy, budget, 100);
    assert(full);
    assert(full.value().updates.size() == 1);
    assert(full.value().updates.front().update_delta_ms == 10);
    assert(full.value().updates.front().catch_up_delta_ms == 0);
    assert(full.value().updates.front().remaining_delta_ms == 90);
    assert(full.value().remaining_catch_up_count == 1);
    assert(full.value().budget_exhausted);
}

void test_temporal_lod_stress_backlog_clears_in_two_ticks() {
    simulation::SimulationLodPolicy policy;
    policy.simplified_tick_interval_ms = 100;

    simulation::SimulationTickBudget budget;
    budget.maximum_updates_per_tick = 2;
    budget.maximum_work_units_per_tick = 2;
    budget.maximum_full_catch_up_delta_ms_per_update = 0;
    budget.maximum_simplified_catch_up_delta_ms_per_update = 100;
    budget.maximum_sleeping_catch_up_delta_ms_per_update = 100;
    budget.maximum_catch_up_delta_ms_per_tick = 200;

    std::vector<simulation::SimulationSubject> subjects{
        make_lod_subject(1, simulation::SimulationLod::simplified, 0),
        make_lod_subject(2, simulation::SimulationLod::simplified, 0),
        make_lod_subject(3, simulation::SimulationLod::simplified, 0),
        make_lod_subject(4, simulation::SimulationLod::simplified, 0),
    };

    auto first =
        simulation::SimulationLodPlanner::plan_budgeted_frame(subjects, {}, policy, budget, 200);
    assert(first);
    assert(first.value().updates.size() == 2);
    assert(first.value().deferred_due_count == 2);
    for (const auto& update : first.value().updates) {
        subjects[update.decision_index].last_update_time_ms = update.next_update_time_ms;
    }

    auto second =
        simulation::SimulationLodPlanner::plan_budgeted_frame(subjects, {}, policy, budget, 201);
    assert(second);
    assert(second.value().updates.size() == 2);
    assert(second.value().deferred_due_count == 0);
    assert(!second.value().budget_exhausted);
    for (const auto& update : second.value().updates) {
        subjects[update.decision_index].last_update_time_ms = update.next_update_time_ms;
    }

    auto cleared =
        simulation::SimulationLodPlanner::plan_budgeted_frame(subjects, {}, policy, budget, 201);
    assert(cleared);
    assert(cleared.value().frame.due_tick_count == 0);
    assert(cleared.value().updates.empty());
    assert(!cleared.value().budget_exhausted);
}

void test_temporal_lod_budget_rejects_permanent_starvation_and_duplicate_identity() {
    simulation::SimulationLodPolicy policy;
    policy.simplified_tick_interval_ms = 100;

    simulation::SimulationTickBudget budget;
    budget.maximum_updates_per_tick = 1;
    budget.maximum_work_units_per_tick = 1;
    budget.maximum_full_catch_up_delta_ms_per_update = 0;
    budget.maximum_simplified_catch_up_delta_ms_per_update = 0;
    budget.maximum_sleeping_catch_up_delta_ms_per_update = 0;
    budget.maximum_catch_up_delta_ms_per_tick = 0;

    auto oversized = simulation::SimulationLodPlanner::plan_budgeted_frame(
        {make_lod_subject(1, simulation::SimulationLod::simplified, 0, 2)}, {}, policy, budget,
        100);
    assert(!oversized);
    assert(oversized.error().code == "simulation.update_exceeds_work_budget");

    auto duplicate = simulation::SimulationLodPlanner::plan_budgeted_frame(
        {make_lod_subject(1, simulation::SimulationLod::simplified, 0),
         make_lod_subject(1, simulation::SimulationLod::simplified, 0)},
        {}, policy, budget, 100);
    assert(!duplicate);
    assert(duplicate.error().code == "simulation.duplicate_subject");

    auto missing_runtime = make_lod_subject(2, simulation::SimulationLod::simplified, 0);
    missing_runtime.save_id = {};
    missing_runtime.persistent = false;
    auto missing = simulation::SimulationLodPlanner::plan_budgeted_frame({missing_runtime}, {},
                                                                         policy, budget, 100);
    assert(!missing);
    assert(missing.error().code == "simulation.missing_runtime_handle");

    budget.maximum_simplified_catch_up_delta_ms_per_update = 1;
    assert(!budget.validate());
    assert(budget.validate().error().code == "simulation.invalid_catch_up_budget");

    policy.full_tick_interval_ms = 1000;
    policy.simplified_tick_interval_ms = 100;
    assert(!policy.validate());
    assert(policy.validate().error().code == "simulation.invalid_tick_interval_order");
}

} // namespace

int main() {
    test_fixed_step_clock_is_bounded_and_deterministic();
    test_tick_events_have_explicit_lifetime();
    test_scheduler_orders_phases_and_dependencies();
    test_scheduler_rejects_invalid_graphs_and_reports_failures();
    test_temporal_lod_budget_is_deadline_ordered_and_hard_bounded();
    test_temporal_lod_budget_has_stable_ties_and_preserves_fixed_full_steps();
    test_temporal_lod_stress_backlog_clears_in_two_ticks();
    test_temporal_lod_budget_rejects_permanent_starvation_and_duplicate_identity();
    return 0;
}
