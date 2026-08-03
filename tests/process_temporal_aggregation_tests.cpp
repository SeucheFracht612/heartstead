#include "engine/processes/process_temporal_aggregation.hpp"
#include "engine/world/world_state.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

using namespace heartstead;

namespace {

const core::PrototypeId process_prototype = *core::PrototypeId::parse("test:processes/temporal");

processes::ProcessInstance make_process(std::uint64_t id, simulation::WorldTick required_work) {
    auto process = processes::ProcessRuntime::create(core::ProcessId::from_value(id),
                                                     core::SaveId::from_value(1'000 + id),
                                                     process_prototype, 0, required_work);
    assert(process);
    return std::move(process).value();
}

processes::TemporalProcessModifierResolver constant_rate(std::int64_t rate_per_mille) {
    return [rate_per_mille](const processes::ProcessInstance&) {
        processes::ProcessModifiers modifiers;
        modifiers.quality_rate_per_mille = rate_per_mille;
        return core::Result<processes::ProcessModifiers>::success(modifiers);
    };
}

std::int64_t varied_rate(const processes::ProcessInstance& process) {
    constexpr std::int64_t rates[]{250, 500, 1000, 2000};
    return rates[process.process_id.value() % 4];
}

void test_config_rejects_unbounded_or_incoherent_limits() {
    processes::ProcessTemporalAggregationConfig config;
    assert(config.validate());

    config.maximum_events_per_tick = 0;
    assert(!config.validate());
    config.maximum_events_per_tick = 1;
    config.stalled_reevaluation_interval_ticks = 0;
    assert(!config.validate());
    config.stalled_reevaluation_interval_ticks = 1;
    config.maximum_catch_up_ticks_per_event = 2;
    config.maximum_catch_up_ticks_per_tick = 1;
    assert(!config.validate());
}

void test_due_events_use_stable_id_order_and_hard_event_budget() {
    world::ProcessDatabase database;
    for (const auto id : std::vector<std::uint64_t>{4, 1, 3, 2}) {
        assert(database.insert(make_process(id, 100)));
    }
    assert(database.insertion_order().size() == 4);
    assert(database.insertion_order().front() == core::ProcessId::from_value(4));

    processes::ProcessTemporalAggregationConfig config;
    config.maximum_admissions_per_tick = 4;
    config.maximum_events_per_tick = 2;
    processes::ProcessTemporalAggregationController controller(config);

    auto first = controller.update(database, 100, constant_rate(1000));
    assert(first);
    assert(first.value().admission_count == 4);
    assert(first.value().dispatched_event_count == 2);
    assert(first.value().completed_process_count == 2);
    assert(first.value().transitions.size() == 2);
    assert(first.value().transitions[0].process_id == core::ProcessId::from_value(1));
    assert(first.value().transitions[1].process_id == core::ProcessId::from_value(2));
    assert(first.value().transitions[0].owner_id == core::SaveId::from_value(1'001));
    assert(first.value().transitions[0].previous_work_ticks == 0);
    assert(first.value().transitions[0].current_work_ticks == 100);
    assert(first.value().transitions[0].current_state == processes::ProcessState::complete);
    assert(first.value().event_budget_exhausted);
    assert(database.find(core::ProcessId::from_value(1))->is_complete());
    assert(database.find(core::ProcessId::from_value(2))->is_complete());
    assert(!database.find(core::ProcessId::from_value(3))->is_complete());
    assert(!database.find(core::ProcessId::from_value(4))->is_complete());

    auto second = controller.update(database, 100, constant_rate(1000));
    assert(second);
    assert(second.value().dispatched_event_count == 2);
    assert(second.value().completed_process_count == 2);
    assert(second.value().transitions.size() == 2);
    assert(second.value().transitions[0].process_id == core::ProcessId::from_value(3));
    assert(second.value().transitions[1].process_id == core::ProcessId::from_value(4));
    assert(!second.value().event_budget_exhausted);
    assert(second.value().active_event_count == 0);
    assert(database.find(core::ProcessId::from_value(3))->is_complete());
    assert(database.find(core::ProcessId::from_value(4))->is_complete());
}

void test_admission_and_catch_up_backlogs_are_bounded() {
    world::ProcessDatabase database;
    assert(database.insert(make_process(1, 100)));
    assert(database.insert(make_process(2, 100)));

    processes::ProcessTemporalAggregationConfig config;
    config.maximum_admissions_per_tick = 1;
    config.maximum_events_per_tick = 1;
    config.maximum_catch_up_ticks_per_event = 50;
    config.maximum_catch_up_ticks_per_tick = 50;
    processes::ProcessTemporalAggregationController controller(config);

    auto seeded = controller.update(database, 0, constant_rate(1000));
    assert(seeded);
    assert(seeded.value().admission_budget_exhausted);
    assert(seeded.value().unadmitted_process_count == 1);

    auto seeded_second = controller.update(database, 0, constant_rate(1000));
    assert(seeded_second);
    assert(seeded_second.value().admission_count == 1);
    assert(!seeded_second.value().admission_budget_exhausted);

    auto slowed = controller.update(database, 1'000, constant_rate(100));
    assert(slowed);
    assert(slowed.value().admission_count == 0);
    assert(slowed.value().dispatched_event_count == 1);
    assert(slowed.value().catch_up_delta_ticks == 50);
    assert(slowed.value().maximum_lateness_ticks == 900);
    assert(slowed.value().event_budget_exhausted);
    assert(database.find(core::ProcessId::from_value(1))->last_eval == 150);
    assert(database.find(core::ProcessId::from_value(1))->accrued_work_ticks == 15);

    auto second_progress = controller.update(database, 1'000, constant_rate(100));
    assert(second_progress);
    assert(second_progress.value().catch_up_delta_ticks == 50);
    assert(database.find(core::ProcessId::from_value(2))->last_eval == 150);
    assert(!database.find(core::ProcessId::from_value(1))->is_complete());

    auto first_complete = controller.update(database, 1'000, constant_rate(100));
    assert(first_complete);
    assert(first_complete.value().completed_process_count == 1);
    assert(database.find(core::ProcessId::from_value(1))->is_complete());

    auto second_complete = controller.update(database, 1'000, constant_rate(100));
    assert(second_complete);
    assert(second_complete.value().completed_process_count == 1);
    assert(database.find(core::ProcessId::from_value(2))->is_complete());
    assert(!second_complete.value().event_budget_exhausted);
}

void test_stalled_processes_poll_at_a_bounded_interval() {
    world::ProcessDatabase database;
    assert(database.insert(make_process(1, 100)));

    processes::ProcessTemporalAggregationConfig config;
    config.stalled_reevaluation_interval_ticks = 20;
    processes::ProcessTemporalAggregationController controller(config);

    auto initial = controller.update(database, 0, constant_rate(0));
    assert(initial && initial.value().active_event_count == 1);
    auto before_due = controller.update(database, 19, constant_rate(0));
    assert(before_due && before_due.value().dispatched_event_count == 0);
    auto stalled = controller.update(database, 20, constant_rate(0));
    assert(stalled && stalled.value().evaluated_process_count == 1);
    assert(stalled.value().transitions.size() == 1);
    assert(stalled.value().transitions.front().previous_work_ticks == 0);
    assert(stalled.value().transitions.front().current_work_ticks == 0);
    assert(stalled.value().transitions.front().current_last_eval == 20);
    assert(database.find(core::ProcessId::from_value(1))->last_eval == 20);
    assert(database.find(core::ProcessId::from_value(1))->accrued_work_ticks == 0);

    auto powered = controller.update(database, 40, constant_rate(1000));
    assert(powered && powered.value().evaluated_process_count == 1);
    assert(database.find(core::ProcessId::from_value(1))->accrued_work_ticks == 20);
    auto completed = controller.update(database, 120, constant_rate(1000));
    assert(completed && completed.value().completed_process_count == 1);
    assert(database.find(core::ProcessId::from_value(1))->is_complete());
}

void test_external_evaluation_invalidates_prediction_without_duplicate_work() {
    world::ProcessDatabase database;
    assert(database.insert(make_process(1, 100)));
    processes::ProcessTemporalAggregationController controller;
    assert(controller.update(database, 0, constant_rate(1000)));

    auto* process = database.find(core::ProcessId::from_value(1));
    assert(process != nullptr);
    assert(processes::ProcessRuntime::advance(*process, 50, {}));
    auto stale = controller.update(database, 100, constant_rate(1000));
    assert(stale);
    assert(stale.value().stale_event_count == 1);
    assert(stale.value().evaluated_process_count == 0);
    assert(stale.value().event_budget_exhausted);

    auto completed = controller.update(database, 100, constant_rate(1000));
    assert(completed && completed.value().completed_process_count == 1);
    assert(process->is_complete());
}

void test_failed_batch_is_atomic_and_retryable() {
    world::ProcessDatabase database;
    assert(database.insert(make_process(1, 100)));
    assert(database.insert(make_process(2, 100)));
    processes::ProcessTemporalAggregationController controller;
    assert(controller.update(database, 0, constant_rate(1000)));

    bool inject_failure = true;
    const processes::TemporalProcessModifierResolver resolver =
        [&inject_failure](const processes::ProcessInstance& process) {
            if (inject_failure && process.process_id == core::ProcessId::from_value(2)) {
                return core::Result<processes::ProcessModifiers>::failure(
                    "test.modifier_failure", "injected modifier failure");
            }
            return core::Result<processes::ProcessModifiers>::success({});
        };

    auto failed = controller.update(database, 100, resolver);
    assert(!failed);
    for (const auto* process : database.records()) {
        assert(process->last_eval == 0);
        assert(process->accrued_work_ticks == 0);
        assert(!process->is_complete());
    }

    inject_failure = false;
    auto retried = controller.update(database, 100, resolver);
    assert(retried);
    assert(retried.value().completed_process_count == 2);
    assert(retried.value().active_event_count == 0);
}

void test_aggregate_outcomes_match_direct_timestamp_evaluation() {
    constexpr std::uint64_t process_count = 512;
    constexpr simulation::WorldTick final_time = 50'000;
    world::ProcessDatabase database;
    std::vector<processes::ProcessInstance> reference;
    reference.reserve(process_count);
    for (std::uint64_t id = 1; id <= process_count; ++id) {
        auto process = make_process(id, 100 + id * 7);
        reference.push_back(process);
        assert(database.insert(std::move(process)));
    }

    const processes::TemporalProcessModifierResolver resolver =
        [](const processes::ProcessInstance& process) {
            processes::ProcessModifiers modifiers;
            modifiers.quality_rate_per_mille = varied_rate(process);
            return core::Result<processes::ProcessModifiers>::success(modifiers);
        };
    for (auto& process : reference) {
        processes::ProcessModifiers modifiers;
        modifiers.quality_rate_per_mille = varied_rate(process);
        assert(processes::ProcessRuntime::advance(process, final_time, modifiers));
    }

    processes::ProcessTemporalAggregationConfig config;
    config.maximum_admissions_per_tick = process_count;
    config.maximum_events_per_tick = 37;
    processes::ProcessTemporalAggregationController controller(config);
    auto seeded = controller.update(database, 0, resolver);
    assert(seeded);
    assert(seeded.value().evaluated_process_count == 0);
    assert(seeded.value().active_event_count == process_count);

    std::uint64_t total_evaluations = 0;
    std::uint64_t total_transitions = 0;
    for (std::uint32_t tick = 0; tick < 32 && controller.active_event_count() > 0; ++tick) {
        auto result = controller.update(database, final_time, resolver);
        assert(result);
        assert(result.value().dispatched_event_count <= config.maximum_events_per_tick);
        total_evaluations += result.value().evaluated_process_count;
        total_transitions += result.value().transitions.size();
    }
    assert(controller.active_event_count() == 0);
    assert(total_evaluations == process_count);
    assert(total_transitions == process_count);
    for (const auto& expected : reference) {
        const auto* actual = database.find(expected.process_id);
        assert(actual != nullptr);
        assert(actual->state == expected.state);
        assert(actual->accrued_work_ticks == expected.accrued_work_ticks);
        assert(actual->is_complete());
    }
}

void test_tracking_and_time_guards_fail_closed() {
    world::ProcessDatabase database;
    assert(database.insert(make_process(1, 100)));
    assert(database.insert(make_process(2, 100)));

    processes::ProcessTemporalAggregationConfig config;
    config.maximum_tracked_processes = 1;
    processes::ProcessTemporalAggregationController bounded(config);
    auto exceeded = bounded.update(database, 0, constant_rate(1000));
    assert(!exceeded);
    assert(exceeded.error().code == "process_temporal.tracking_budget_exceeded");

    processes::ProcessTemporalAggregationController controller;
    world::ProcessDatabase one;
    assert(one.insert(make_process(1, 100)));
    assert(controller.update(one, 10, constant_rate(1000)));
    auto reversed = controller.update(one, 9, constant_rate(1000));
    assert(!reversed);
    assert(reversed.error().code == "process_temporal.time_reversed");
}

} // namespace

int main() {
    test_config_rejects_unbounded_or_incoherent_limits();
    test_due_events_use_stable_id_order_and_hard_event_budget();
    test_admission_and_catch_up_backlogs_are_bounded();
    test_stalled_processes_poll_at_a_bounded_interval();
    test_external_evaluation_invalidates_prediction_without_duplicate_work();
    test_failed_batch_is_atomic_and_retryable();
    test_aggregate_outcomes_match_direct_timestamp_evaluation();
    test_tracking_and_time_guards_fail_closed();
    return 0;
}
