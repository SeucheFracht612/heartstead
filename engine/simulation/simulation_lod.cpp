#include "engine/simulation/simulation_lod.hpp"

#include <algorithm>
#include <compare>
#include <limits>
#include <utility>

namespace heartstead::simulation {

namespace {

[[nodiscard]] std::uint64_t ordered_axis_bits(std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << 63U);
}

[[nodiscard]] std::uint64_t abs_diff(std::int64_t a, std::int64_t b) noexcept {
    const auto left = ordered_axis_bits(a);
    const auto right = ordered_axis_bits(b);
    if (left >= right) {
        return left - right;
    }
    return right - left;
}

[[nodiscard]] std::uint64_t saturated_square(std::uint64_t value) noexcept {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (value != 0 && value > max / value) {
        return max;
    }
    return value * value;
}

[[nodiscard]] std::uint64_t saturated_add(std::uint64_t left, std::uint64_t right) noexcept {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    if (left > max - right) {
        return max;
    }
    return left + right;
}

[[nodiscard]] std::uint64_t squared_distance(SimulationCoord a, SimulationCoord b) noexcept {
    const auto dx = saturated_square(abs_diff(a.x, b.x));
    const auto dy = saturated_square(abs_diff(a.y, b.y));
    const auto dz = saturated_square(abs_diff(a.z, b.z));
    return saturated_add(saturated_add(dx, dy), dz);
}

[[nodiscard]] std::uint64_t radius_squared(std::uint32_t radius) noexcept {
    return saturated_square(static_cast<std::uint64_t>(radius));
}

[[nodiscard]] std::uint64_t
nearest_distance_squared(SimulationCoord subject_coord,
                         const std::vector<SimulationViewer>& viewers) noexcept {
    if (viewers.empty()) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    auto nearest = std::numeric_limits<std::uint64_t>::max();
    for (const auto& viewer : viewers) {
        nearest = std::min(nearest, squared_distance(subject_coord, viewer.coord));
    }
    return nearest;
}

[[nodiscard]] core::Status validate_subject(const SimulationSubject& subject) {
    if (subject.persistent && !subject.save_id.is_valid()) {
        return core::Status::failure("simulation.missing_save_id",
                                     "persistent simulation subjects need stable save ids");
    }
    if (!subject.persistent && subject.save_id.is_valid()) {
        return core::Status::failure("simulation.unexpected_save_id",
                                     "non-persistent simulation subjects must not claim a save id");
    }
    if (subject.kind == SimulationSubjectKind::process_owner && !subject.process_id.is_valid()) {
        return core::Status::failure("simulation.missing_process_id",
                                     "process-owner simulation subjects need stable process ids");
    }
    if (!subject.persistent && !subject.runtime_handle.is_valid()) {
        return core::Status::failure(
            "simulation.missing_runtime_handle",
            "non-persistent simulation subjects need stable runtime handles");
    }
    if (subject.estimated_update_work_units == 0) {
        return core::Status::failure("simulation.invalid_update_work",
                                     "simulation update work estimates must be positive");
    }
    return core::Status::ok();
}

[[nodiscard]] SimulationLod distance_lod(std::uint64_t nearest_squared,
                                         const SimulationLodPolicy& policy) noexcept {
    if (nearest_squared <= radius_squared(policy.full_radius)) {
        return SimulationLod::full;
    }
    if (nearest_squared <= radius_squared(policy.simplified_radius)) {
        return SimulationLod::simplified;
    }
    return SimulationLod::unloaded;
}

[[nodiscard]] WorldTick tick_interval_for(SimulationLod lod,
                                          const SimulationLodPolicy& policy) noexcept {
    switch (lod) {
    case SimulationLod::full:
        return policy.full_tick_interval_ms;
    case SimulationLod::simplified:
        return policy.simplified_tick_interval_ms;
    case SimulationLod::sleeping:
        return policy.sleeping_tick_interval_ms;
    case SimulationLod::unloaded:
        return 0;
    }
    return 0;
}

void increment_lod_count(SimulationFramePlan& plan, SimulationLod lod) noexcept {
    switch (lod) {
    case SimulationLod::full:
        ++plan.full_count;
        break;
    case SimulationLod::simplified:
        ++plan.simplified_count;
        break;
    case SimulationLod::sleeping:
        ++plan.sleeping_count;
        break;
    case SimulationLod::unloaded:
        ++plan.unloaded_count;
        break;
    }
}

[[nodiscard]] WorldTick
maximum_catch_up_delta_per_update(SimulationLod lod, const SimulationTickBudget& budget) noexcept {
    switch (lod) {
    case SimulationLod::full:
        return budget.maximum_full_catch_up_delta_ms_per_update;
    case SimulationLod::simplified:
        return budget.maximum_simplified_catch_up_delta_ms_per_update;
    case SimulationLod::sleeping:
        return budget.maximum_sleeping_catch_up_delta_ms_per_update;
    case SimulationLod::unloaded:
        return 0;
    }
    return 0;
}

enum class SimulationIdentitySource : std::uint8_t {
    process,
    save,
    runtime,
};

struct SimulationIdentity {
    SimulationSubjectKind kind = SimulationSubjectKind::custom;
    SimulationIdentitySource source = SimulationIdentitySource::runtime;
    std::uint64_t value = 0;

    friend auto operator<=>(const SimulationIdentity&, const SimulationIdentity&) = default;
};

[[nodiscard]] SimulationIdentity identity_for(const SimulationLodDecision& decision) noexcept {
    if (decision.kind == SimulationSubjectKind::process_owner) {
        return {decision.kind, SimulationIdentitySource::process, decision.process_id.value()};
    }
    if (decision.save_id.is_valid()) {
        return {decision.kind, SimulationIdentitySource::save, decision.save_id.value()};
    }
    return {decision.kind, SimulationIdentitySource::runtime, decision.runtime_handle.value()};
}

[[nodiscard]] std::uint8_t lod_priority(SimulationLod lod) noexcept {
    switch (lod) {
    case SimulationLod::full:
        return 0;
    case SimulationLod::simplified:
        return 1;
    case SimulationLod::sleeping:
        return 2;
    case SimulationLod::unloaded:
        return 3;
    }
    return 3;
}

void accumulate_saturated(std::uint64_t& total, std::uint64_t value, bool& saturated) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (total > maximum - value) {
        total = maximum;
        saturated = true;
        return;
    }
    total += value;
}

} // namespace

core::Status SimulationLodPolicy::validate() const {
    if (full_radius > simplified_radius) {
        return core::Status::failure("simulation.invalid_radius",
                                     "full simulation radius cannot exceed simplified radius");
    }
    if (full_tick_interval_ms == 0 || simplified_tick_interval_ms == 0 ||
        sleeping_tick_interval_ms == 0) {
        return core::Status::failure("simulation.invalid_tick_interval",
                                     "simulation tick intervals must be positive");
    }
    if (full_tick_interval_ms > simplified_tick_interval_ms ||
        simplified_tick_interval_ms > sleeping_tick_interval_ms) {
        return core::Status::failure(
            "simulation.invalid_tick_interval_order",
            "simulation tick intervals must not decrease at lower-detail levels");
    }
    return core::Status::ok();
}

core::Status SimulationTickBudget::validate() const {
    constexpr std::uint32_t maximum_update_limit = 1'000'000;
    if (maximum_updates_per_tick == 0 || maximum_updates_per_tick > maximum_update_limit) {
        return core::Status::failure(
            "simulation.invalid_update_budget",
            "simulation update budget must be between 1 and 1000000 updates per tick");
    }
    if (maximum_work_units_per_tick == 0) {
        return core::Status::failure("simulation.invalid_work_budget",
                                     "simulation work-unit budget must be positive");
    }
    if (maximum_full_catch_up_delta_ms_per_update > maximum_catch_up_delta_ms_per_tick ||
        maximum_simplified_catch_up_delta_ms_per_update > maximum_catch_up_delta_ms_per_tick ||
        maximum_sleeping_catch_up_delta_ms_per_update > maximum_catch_up_delta_ms_per_tick) {
        return core::Status::failure(
            "simulation.invalid_catch_up_budget",
            "per-update simulation catch-up limits cannot exceed the per-tick limit");
    }
    return core::Status::ok();
}

std::size_t SimulationFramePlan::count(SimulationLod lod) const noexcept {
    switch (lod) {
    case SimulationLod::full:
        return full_count;
    case SimulationLod::simplified:
        return simplified_count;
    case SimulationLod::sleeping:
        return sleeping_count;
    case SimulationLod::unloaded:
        return unloaded_count;
    }
    return 0;
}

const char* to_string(SimulationLod lod) noexcept {
    switch (lod) {
    case SimulationLod::full:
        return "full";
    case SimulationLod::simplified:
        return "simplified";
    case SimulationLod::sleeping:
        return "sleeping";
    case SimulationLod::unloaded:
        return "unloaded";
    }
    return "unknown";
}

const char* to_string(SimulationSubjectKind kind) noexcept {
    switch (kind) {
    case SimulationSubjectKind::entity:
        return "entity";
    case SimulationSubjectKind::build_piece:
        return "build_piece";
    case SimulationSubjectKind::assembly:
        return "assembly";
    case SimulationSubjectKind::process_owner:
        return "process_owner";
    case SimulationSubjectKind::network:
        return "network";
    case SimulationSubjectKind::chunk_region:
        return "chunk_region";
    case SimulationSubjectKind::custom:
        return "custom";
    }
    return "unknown";
}

core::Result<SimulationLodDecision>
SimulationLodPlanner::classify(const SimulationSubject& subject,
                               const std::vector<SimulationViewer>& viewers,
                               const SimulationLodPolicy& policy, WorldTick now_ms) {
    const auto policy_status = policy.validate();
    if (!policy_status) {
        return core::Result<SimulationLodDecision>::failure(policy_status.error().code,
                                                            policy_status.error().message);
    }

    const auto subject_status = validate_subject(subject);
    if (!subject_status) {
        return core::Result<SimulationLodDecision>::failure(subject_status.error().code,
                                                            subject_status.error().message);
    }

    if (now_ms < subject.last_update_time_ms) {
        return core::Result<SimulationLodDecision>::failure("simulation.time_reversed",
                                                            "simulation time cannot move backward");
    }

    SimulationLodDecision decision;
    decision.save_id = subject.save_id;
    decision.runtime_handle = subject.runtime_handle;
    decision.process_id = subject.process_id;
    decision.kind = subject.kind;
    decision.nearest_viewer_distance_squared = nearest_distance_squared(subject.coord, viewers);
    decision.elapsed_since_update_ms = now_ms - subject.last_update_time_ms;
    decision.estimated_update_work_units = subject.estimated_update_work_units;

    if (subject.forced_lod.has_value()) {
        decision.lod = subject.forced_lod.value();
    } else {
        decision.lod = distance_lod(decision.nearest_viewer_distance_squared, policy);
        if (subject.sleeping && decision.lod != SimulationLod::unloaded) {
            decision.lod = SimulationLod::sleeping;
        }
    }

    if (decision.lod == SimulationLod::unloaded) {
        decision.offline_delta_ms = decision.elapsed_since_update_ms;
        return core::Result<SimulationLodDecision>::success(decision);
    }

    const auto interval = tick_interval_for(decision.lod, policy);
    decision.tick_interval_ms = interval;
    decision.due_for_tick = decision.elapsed_since_update_ms >= interval;
    return core::Result<SimulationLodDecision>::success(decision);
}

core::Result<SimulationFramePlan>
SimulationLodPlanner::plan_frame(const std::vector<SimulationSubject>& subjects,
                                 const std::vector<SimulationViewer>& viewers,
                                 const SimulationLodPolicy& policy, WorldTick now_ms) {
    SimulationFramePlan plan;
    plan.decisions.reserve(subjects.size());

    for (const auto& subject : subjects) {
        auto decision = classify(subject, viewers, policy, now_ms);
        if (!decision) {
            return core::Result<SimulationFramePlan>::failure(decision.error().code,
                                                              decision.error().message);
        }

        increment_lod_count(plan, decision.value().lod);
        if (decision.value().due_for_tick) {
            ++plan.due_tick_count;
        }
        plan.decisions.push_back(std::move(decision).value());
    }

    return core::Result<SimulationFramePlan>::success(std::move(plan));
}

core::Result<BudgetedSimulationFramePlan> SimulationLodPlanner::plan_budgeted_frame(
    const std::vector<SimulationSubject>& subjects, const std::vector<SimulationViewer>& viewers,
    const SimulationLodPolicy& policy, const SimulationTickBudget& budget, WorldTick now_ms) {
    const auto budget_status = budget.validate();
    if (!budget_status) {
        return core::Result<BudgetedSimulationFramePlan>::failure(budget_status.error().code,
                                                                  budget_status.error().message);
    }

    auto frame = plan_frame(subjects, viewers, policy, now_ms);
    if (!frame) {
        return core::Result<BudgetedSimulationFramePlan>::failure(frame.error().code,
                                                                  frame.error().message);
    }

    BudgetedSimulationFramePlan plan;
    plan.frame = std::move(frame).value();
    plan.budget = budget;
    plan.now_ms = now_ms;

    std::vector<std::pair<SimulationIdentity, std::size_t>> identities;
    identities.reserve(plan.frame.decisions.size());
    for (std::size_t index = 0; index < plan.frame.decisions.size(); ++index) {
        identities.emplace_back(identity_for(plan.frame.decisions[index]), index);
    }
    std::ranges::sort(identities,
                      [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    for (std::size_t index = 1; index < identities.size(); ++index) {
        if (identities[index - 1].first == identities[index].first) {
            return core::Result<BudgetedSimulationFramePlan>::failure(
                "simulation.duplicate_subject",
                "budgeted simulation frame contains a duplicate stable subject identity");
        }
    }

    struct Candidate {
        std::size_t decision_index = 0;
        WorldTick lateness_ms = 0;
        SimulationIdentity identity;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(plan.frame.due_tick_count);
    for (std::size_t index = 0; index < plan.frame.decisions.size(); ++index) {
        const auto& decision = plan.frame.decisions[index];
        if (!decision.due_for_tick) {
            continue;
        }
        if (decision.tick_interval_ms == 0 ||
            decision.elapsed_since_update_ms < decision.tick_interval_ms) {
            return core::Result<BudgetedSimulationFramePlan>::failure(
                "simulation.invalid_due_decision",
                "due simulation decision must have a positive elapsed tick interval");
        }
        if (decision.estimated_update_work_units > budget.maximum_work_units_per_tick) {
            return core::Result<BudgetedSimulationFramePlan>::failure(
                "simulation.update_exceeds_work_budget",
                "one simulation subject exceeds the complete per-tick work-unit budget");
        }

        const auto lateness = decision.elapsed_since_update_ms - decision.tick_interval_ms;
        candidates.push_back({index, lateness, identity_for(decision)});
        accumulate_saturated(plan.due_work_units, decision.estimated_update_work_units,
                             plan.counters_saturated);
        plan.maximum_lateness_ms = std::max(plan.maximum_lateness_ms, lateness);
        if (lateness > 0) {
            ++plan.catch_up_due_count;
        }
    }

    std::ranges::sort(candidates, [&plan](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.lateness_ms != rhs.lateness_ms) {
            return lhs.lateness_ms > rhs.lateness_ms;
        }
        const auto lhs_lod = plan.frame.decisions[lhs.decision_index].lod;
        const auto rhs_lod = plan.frame.decisions[rhs.decision_index].lod;
        if (lod_priority(lhs_lod) != lod_priority(rhs_lod)) {
            return lod_priority(lhs_lod) < lod_priority(rhs_lod);
        }
        return lhs.identity < rhs.identity;
    });

    plan.updates.reserve(std::min<std::size_t>(candidates.size(), budget.maximum_updates_per_tick));
    for (const auto& candidate : candidates) {
        const auto& decision = plan.frame.decisions[candidate.decision_index];
        const auto work_units = static_cast<std::uint64_t>(decision.estimated_update_work_units);
        const auto update_limit_reached = plan.updates.size() >= budget.maximum_updates_per_tick;
        const auto work_limit_reached =
            plan.scheduled_work_units > budget.maximum_work_units_per_tick - work_units;
        if (update_limit_reached || work_limit_reached) {
            ++plan.deferred_due_count;
            accumulate_saturated(plan.deferred_work_units, work_units, plan.counters_saturated);
            accumulate_saturated(plan.deferred_delta_ms, decision.elapsed_since_update_ms,
                                 plan.counters_saturated);
            accumulate_saturated(plan.deferred_catch_up_delta_ms, candidate.lateness_ms,
                                 plan.counters_saturated);
            if (candidate.lateness_ms > 0) {
                ++plan.remaining_catch_up_count;
            }
            continue;
        }

        const auto remaining_catch_up_budget =
            budget.maximum_catch_up_delta_ms_per_tick - plan.scheduled_catch_up_delta_ms;
        const auto catch_up_delta = std::min(
            {candidate.lateness_ms, maximum_catch_up_delta_per_update(decision.lod, budget),
             remaining_catch_up_budget});
        const auto update_delta = decision.tick_interval_ms + catch_up_delta;
        const auto remaining_delta = decision.elapsed_since_update_ms - update_delta;
        const auto previous_update_time = now_ms - decision.elapsed_since_update_ms;

        plan.updates.push_back({candidate.decision_index, update_delta, catch_up_delta,
                                remaining_delta, previous_update_time + update_delta,
                                decision.estimated_update_work_units});
        plan.scheduled_work_units += work_units;
        accumulate_saturated(plan.scheduled_delta_ms, update_delta, plan.counters_saturated);
        accumulate_saturated(plan.scheduled_catch_up_delta_ms, catch_up_delta,
                             plan.counters_saturated);
        if (remaining_delta >= decision.tick_interval_ms) {
            accumulate_saturated(plan.deferred_delta_ms, remaining_delta, plan.counters_saturated);
            accumulate_saturated(plan.deferred_catch_up_delta_ms,
                                 remaining_delta - decision.tick_interval_ms,
                                 plan.counters_saturated);
            ++plan.remaining_catch_up_count;
        }
    }

    plan.budget_exhausted = plan.deferred_due_count > 0 || plan.remaining_catch_up_count > 0;
    return core::Result<BudgetedSimulationFramePlan>::success(std::move(plan));
}

} // namespace heartstead::simulation
