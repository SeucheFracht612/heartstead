#include "engine/processes/process_temporal_aggregation.hpp"

#include "engine/profiling/profiler.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace heartstead::processes {

namespace {

constexpr std::uint32_t maximum_per_tick_limit = 1'000'000;

[[nodiscard]] simulation::WorldTick saturated_add(simulation::WorldTick left,
                                                  simulation::WorldTick right,
                                                  bool* saturated = nullptr) noexcept {
    constexpr auto maximum = std::numeric_limits<simulation::WorldTick>::max();
    if (left > maximum - right) {
        if (saturated != nullptr) {
            *saturated = true;
        }
        return maximum;
    }
    return left + right;
}

void accumulate_saturated(simulation::WorldTick& total, simulation::WorldTick value,
                          bool& saturated) noexcept {
    total = saturated_add(total, value, &saturated);
}

[[nodiscard]] simulation::WorldTick
ticks_until_work_complete(simulation::WorldTick remaining_work,
                          std::int64_t rate_per_mille) noexcept {
    if (remaining_work == 0) {
        return 0;
    }
    if (rate_per_mille <= 0) {
        return std::numeric_limits<simulation::WorldTick>::max();
    }

    const auto rate = static_cast<simulation::WorldTick>(rate_per_mille);
    constexpr auto scale = simulation::WorldTick{1000};
    constexpr auto maximum = std::numeric_limits<simulation::WorldTick>::max();
    const auto whole = remaining_work / rate;
    const auto remainder = remaining_work % rate;
    const auto ticks = whole > maximum / scale ? maximum : whole * scale;
    if (ticks == maximum || remainder == 0) {
        return ticks;
    }
    const auto scaled_remainder = remainder * scale;
    const auto partial = scaled_remainder / rate + (scaled_remainder % rate == 0 ? 0 : 1);
    return saturated_add(ticks, partial);
}

struct StagedEvaluation {
    ProcessInstance process;
    ProcessModifiers modifiers;
    simulation::WorldTick target_time = 0;
    ProcessState previous_state = ProcessState::running;
    simulation::WorldTick previous_last_eval = 0;
    simulation::WorldTick previous_work = 0;
};

} // namespace

core::Status ProcessTemporalAggregationConfig::validate() const {
    if (maximum_admissions_per_tick == 0 || maximum_admissions_per_tick > maximum_per_tick_limit) {
        return core::Status::failure(
            "process_temporal.invalid_admission_budget",
            "process temporal admission budget must be between 1 and 1000000");
    }
    if (maximum_events_per_tick == 0 || maximum_events_per_tick > maximum_per_tick_limit) {
        return core::Status::failure("process_temporal.invalid_event_budget",
                                     "process temporal event budget must be between 1 and 1000000");
    }
    if (maximum_tracked_processes == 0) {
        return core::Status::failure("process_temporal.invalid_tracking_budget",
                                     "process temporal tracking budget must be positive");
    }
    if (stalled_reevaluation_interval_ticks == 0) {
        return core::Status::failure("process_temporal.invalid_reevaluation_interval",
                                     "stalled process reevaluation interval must be positive");
    }
    if (maximum_catch_up_ticks_per_event > maximum_catch_up_ticks_per_tick) {
        return core::Status::failure(
            "process_temporal.invalid_catch_up_budget",
            "per-event process catch-up cannot exceed the complete per-tick catch-up budget");
    }
    return core::Status::ok();
}

ProcessTemporalAggregationController::ProcessTemporalAggregationController(
    ProcessTemporalAggregationConfig config)
    : config_(config) {}

bool ProcessTemporalAggregationController::LaterEvent::operator()(const Event& lhs,
                                                                  const Event& rhs) const noexcept {
    if (lhs.due_time != rhs.due_time) {
        return lhs.due_time > rhs.due_time;
    }
    return lhs.process_id.value() > rhs.process_id.value();
}

core::Result<std::optional<ProcessTemporalAggregationController::Event>>
ProcessTemporalAggregationController::predict_event(const ProcessInstance& process,
                                                    ProcessModifiers modifiers,
                                                    simulation::WorldTick not_before) const {
    auto status = process.validate();
    if (!status) {
        return core::Result<std::optional<Event>>::failure(status.error().code,
                                                           status.error().message);
    }
    if (process.state == ProcessState::complete) {
        return core::Result<std::optional<Event>>::success(std::nullopt);
    }

    Event event;
    event.process_id = process.process_id;
    event.expected_last_eval = process.last_eval;
    const auto anchor = std::max(process.last_eval, not_before);
    const auto rate = modifiers.effective_rate_per_mille();
    if (process.state == ProcessState::interrupted || rate <= 0) {
        event.due_time = saturated_add(anchor, config_.stalled_reevaluation_interval_ticks);
    } else {
        const auto delta = ticks_until_work_complete(process.remaining_work_ticks(), rate);
        if (delta == 0) {
            return core::Result<std::optional<Event>>::failure(
                "process_temporal.invalid_completion_prediction",
                "running process predicted a zero-length completion transition");
        }
        event.due_time = saturated_add(process.last_eval, delta);
        event.due_time = std::max(event.due_time, not_before);
    }
    return core::Result<std::optional<Event>>::success(event);
}

core::Result<ProcessTemporalAggregationTickStats> ProcessTemporalAggregationController::update(
    world::ProcessDatabase& processes, simulation::WorldTick world_time,
    const TemporalProcessModifierResolver& modifier_resolver) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("simulation.process_temporal_aggregation");
    const auto config_status = config_.validate();
    if (!config_status) {
        return core::Result<ProcessTemporalAggregationTickStats>::failure(
            config_status.error().code, config_status.error().message);
    }
    if (!modifier_resolver) {
        return core::Result<ProcessTemporalAggregationTickStats>::failure(
            "process_temporal.missing_modifier_resolver",
            "process temporal aggregation requires a modifier resolver");
    }
    if (last_world_time_.has_value() && world_time < last_world_time_.value()) {
        return core::Result<ProcessTemporalAggregationTickStats>::failure(
            "process_temporal.time_reversed", "process temporal world time cannot move backward");
    }

    ProcessTemporalAggregationTickStats stats;
    stats.world_time = world_time;
    const auto insertion_order = processes.insertion_order();
    stats.process_record_count = insertion_order.size();
    if (insertion_order.size() > config_.maximum_tracked_processes) {
        return core::Result<ProcessTemporalAggregationTickStats>::failure(
            "process_temporal.tracking_budget_exceeded",
            "process database exceeds the configured temporal tracking budget");
    }
    if (admitted_process_count_ > insertion_order.size()) {
        return core::Result<ProcessTemporalAggregationTickStats>::failure(
            "process_temporal.database_replaced",
            "process database insertion history regressed while the controller was active");
    }

    const auto remaining_admissions = insertion_order.size() - admitted_process_count_;
    const auto admission_count =
        std::min<std::size_t>(remaining_admissions, config_.maximum_admissions_per_tick);
    std::vector<Event> admitted_events;
    admitted_events.reserve(admission_count);
    for (std::size_t offset = 0; offset < admission_count; ++offset) {
        const auto process_id = insertion_order[admitted_process_count_ + offset];
        const auto* process = processes.find(process_id);
        if (process == nullptr) {
            return core::Result<ProcessTemporalAggregationTickStats>::failure(
                "process_temporal.missing_admitted_process",
                "process insertion history references a missing record");
        }
        if (process->last_eval > world_time) {
            return core::Result<ProcessTemporalAggregationTickStats>::failure(
                "process_temporal.process_from_future",
                "process last evaluation time exceeds authoritative world time");
        }
        auto modifiers = modifier_resolver(*process);
        if (!modifiers) {
            return core::Result<ProcessTemporalAggregationTickStats>::failure(
                modifiers.error().code, modifiers.error().message);
        }
        auto event = predict_event(*process, modifiers.value(), process->last_eval);
        if (!event) {
            return core::Result<ProcessTemporalAggregationTickStats>::failure(
                event.error().code, event.error().message);
        }
        if (event.value().has_value()) {
            admitted_events.push_back(event.value().value());
        }
    }
    for (const auto& event : admitted_events) {
        events_.push(event);
    }
    admitted_process_count_ += admission_count;
    stats.admission_count = static_cast<std::uint32_t>(admission_count);

    std::vector<Event> dispatched;
    dispatched.reserve(config_.maximum_events_per_tick);
    while (dispatched.size() < config_.maximum_events_per_tick && !events_.empty() &&
           events_.top().due_time <= world_time) {
        dispatched.push_back(events_.top());
        events_.pop();
    }
    stats.dispatched_event_count = static_cast<std::uint32_t>(dispatched.size());

    std::vector<Event> replacement_events;
    replacement_events.reserve(dispatched.size());
    std::vector<StagedEvaluation> staged;
    staged.reserve(dispatched.size());
    std::vector<std::optional<Event>> staged_replacements;
    staged_replacements.reserve(dispatched.size());
    std::unordered_set<std::uint64_t> dispatched_ids;
    dispatched_ids.reserve(dispatched.size());
    simulation::WorldTick remaining_catch_up = config_.maximum_catch_up_ticks_per_tick;

    for (const auto& event : dispatched) {
        if (!dispatched_ids.insert(event.process_id.value()).second) {
            for (const auto& restore : dispatched) {
                events_.push(restore);
            }
            return core::Result<ProcessTemporalAggregationTickStats>::failure(
                "process_temporal.duplicate_event",
                "one process owned more than one temporal event in a tick");
        }
        const auto* process = processes.find(event.process_id);
        if (process == nullptr || process->state == ProcessState::complete) {
            ++stats.retired_event_count;
            continue;
        }

        auto modifiers = modifier_resolver(*process);
        if (!modifiers) {
            for (const auto& restore : dispatched) {
                events_.push(restore);
            }
            return core::Result<ProcessTemporalAggregationTickStats>::failure(
                modifiers.error().code, modifiers.error().message);
        }
        if (process->last_eval != event.expected_last_eval) {
            ++stats.stale_event_count;
            auto replacement = predict_event(*process, modifiers.value(), world_time);
            if (!replacement) {
                for (const auto& restore : dispatched) {
                    events_.push(restore);
                }
                return core::Result<ProcessTemporalAggregationTickStats>::failure(
                    replacement.error().code, replacement.error().message);
            }
            if (replacement.value().has_value()) {
                replacement_events.push_back(replacement.value().value());
            }
            continue;
        }

        const auto lateness = world_time - event.due_time;
        stats.maximum_lateness_ticks = std::max(stats.maximum_lateness_ticks, lateness);
        const auto catch_up =
            std::min({lateness, config_.maximum_catch_up_ticks_per_event, remaining_catch_up});
        remaining_catch_up -= catch_up;
        StagedEvaluation evaluation;
        evaluation.process = *process;
        evaluation.modifiers = modifiers.value();
        evaluation.target_time = saturated_add(event.due_time, catch_up);
        evaluation.previous_state = process->state;
        evaluation.previous_last_eval = process->last_eval;
        evaluation.previous_work = process->accrued_work_ticks;
        auto status =
            ProcessRuntime::evaluate(evaluation.process, evaluation.target_time,
                                     evaluation.modifiers, ProcessEvaluationTrigger::state_change);
        if (!status) {
            for (const auto& restore : dispatched) {
                events_.push(restore);
            }
            return core::Result<ProcessTemporalAggregationTickStats>::failure(
                status.error().code, status.error().message);
        }
        auto replacement =
            predict_event(evaluation.process, evaluation.modifiers, evaluation.target_time);
        if (!replacement) {
            for (const auto& restore : dispatched) {
                events_.push(restore);
            }
            return core::Result<ProcessTemporalAggregationTickStats>::failure(
                replacement.error().code, replacement.error().message);
        }
        staged_replacements.push_back(std::move(replacement).value());
        staged.push_back(std::move(evaluation));
        accumulate_saturated(stats.catch_up_delta_ticks, catch_up, stats.counters_saturated);
    }

    std::vector<ProcessInstance*> commit_targets;
    commit_targets.reserve(staged.size());
    for (const auto& evaluation : staged) {
        const auto process_id = evaluation.process.process_id;
        auto* process = processes.find(process_id);
        if (process == nullptr || process->last_eval != evaluation.previous_last_eval ||
            process->state != evaluation.previous_state ||
            process->accrued_work_ticks != evaluation.previous_work) {
            for (const auto& restore : dispatched) {
                events_.push(restore);
            }
            return core::Result<ProcessTemporalAggregationTickStats>::failure(
                "process_temporal.concurrent_process_mutation",
                "process changed while a temporal batch was staged");
        }
        commit_targets.push_back(process);
    }

    stats.transitions.reserve(staged.size());
    for (std::size_t index = 0; index < staged.size(); ++index) {
        auto& evaluation = staged[index];
        auto* process = commit_targets[index];
        const auto delta = evaluation.target_time - evaluation.previous_last_eval;
        accumulate_saturated(stats.evaluated_delta_ticks, delta, stats.counters_saturated);
        ++stats.evaluated_process_count;
        if (evaluation.process.state != evaluation.previous_state ||
            evaluation.process.last_eval != evaluation.previous_last_eval ||
            evaluation.process.accrued_work_ticks != evaluation.previous_work) {
            ++stats.changed_process_count;
            stats.transitions.push_back(ProcessTemporalAggregationTransition{
                evaluation.process.process_id,
                evaluation.process.owner_id,
                evaluation.previous_state,
                evaluation.process.state,
                evaluation.previous_last_eval,
                evaluation.process.last_eval,
                evaluation.previous_work,
                evaluation.process.accrued_work_ticks,
            });
        }
        if (evaluation.previous_state != ProcessState::complete &&
            evaluation.process.state == ProcessState::complete) {
            ++stats.completed_process_count;
        }
        *process = evaluation.process;
        if (staged_replacements[index].has_value()) {
            replacement_events.push_back(staged_replacements[index].value());
        }
    }

    for (const auto& event : replacement_events) {
        events_.push(event);
    }
    last_world_time_ = world_time;
    stats.admitted_process_count = admitted_process_count_;
    stats.active_event_count = events_.size();
    stats.unadmitted_process_count = insertion_order.size() - admitted_process_count_;
    stats.admission_budget_exhausted = stats.unadmitted_process_count > 0;
    stats.event_budget_exhausted = !events_.empty() && events_.top().due_time <= world_time;
    if (stats.event_budget_exhausted) {
        stats.oldest_deferred_lateness_ticks = world_time - events_.top().due_time;
    }
    stats.catch_up_budget_exhausted = remaining_catch_up == 0 && stats.event_budget_exhausted;
    return core::Result<ProcessTemporalAggregationTickStats>::success(stats);
}

const ProcessTemporalAggregationConfig&
ProcessTemporalAggregationController::config() const noexcept {
    return config_;
}

std::size_t ProcessTemporalAggregationController::active_event_count() const noexcept {
    return events_.size();
}

std::size_t ProcessTemporalAggregationController::admitted_process_count() const noexcept {
    return admitted_process_count_;
}

void ProcessTemporalAggregationController::reset() noexcept {
    events_ = {};
    admitted_process_count_ = 0;
    last_world_time_.reset();
}

} // namespace heartstead::processes
