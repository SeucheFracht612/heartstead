#pragma once

#include "engine/core/result.hpp"
#include "engine/processes/process.hpp"
#include "engine/simulation/world_time.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <vector>

namespace heartstead::world {
class ProcessDatabase;
}

namespace heartstead::processes {

using TemporalProcessModifierResolver =
    std::function<core::Result<ProcessModifiers>(const ProcessInstance&)>;

struct ProcessTemporalAggregationConfig {
    std::uint32_t maximum_admissions_per_tick = 256;
    std::uint32_t maximum_events_per_tick = 256;
    std::size_t maximum_tracked_processes = 65'536;
    simulation::WorldTick stalled_reevaluation_interval_ticks = 20;
    simulation::WorldTick maximum_catch_up_ticks_per_event = 1'200;
    simulation::WorldTick maximum_catch_up_ticks_per_tick = 4'800;

    [[nodiscard]] core::Status validate() const;
};

struct ProcessTemporalAggregationTickStats {
    simulation::WorldTick world_time = 0;
    std::size_t process_record_count = 0;
    std::size_t admitted_process_count = 0;
    std::size_t active_event_count = 0;
    std::size_t unadmitted_process_count = 0;
    std::uint32_t admission_count = 0;
    std::uint32_t dispatched_event_count = 0;
    std::uint32_t evaluated_process_count = 0;
    std::uint32_t changed_process_count = 0;
    std::uint32_t completed_process_count = 0;
    std::uint32_t stale_event_count = 0;
    std::uint32_t retired_event_count = 0;
    simulation::WorldTick evaluated_delta_ticks = 0;
    simulation::WorldTick catch_up_delta_ticks = 0;
    simulation::WorldTick maximum_lateness_ticks = 0;
    simulation::WorldTick oldest_deferred_lateness_ticks = 0;
    bool admission_budget_exhausted = false;
    bool event_budget_exhausted = false;
    bool catch_up_budget_exhausted = false;
    bool counters_saturated = false;
};

// Event-driven proxy for timestamp-based production processes. Each admitted non-complete process
// owns exactly one predicted transition. The owner thread calls update after authoritative world
// time advances; no process is touched between events.
class ProcessTemporalAggregationController {
  public:
    explicit ProcessTemporalAggregationController(ProcessTemporalAggregationConfig config = {});

    [[nodiscard]] core::Result<ProcessTemporalAggregationTickStats>
    update(world::ProcessDatabase& processes, simulation::WorldTick world_time,
           const TemporalProcessModifierResolver& modifier_resolver);

    [[nodiscard]] const ProcessTemporalAggregationConfig& config() const noexcept;
    [[nodiscard]] std::size_t active_event_count() const noexcept;
    [[nodiscard]] std::size_t admitted_process_count() const noexcept;
    void reset() noexcept;

  private:
    struct Event {
        simulation::WorldTick due_time = 0;
        core::ProcessId process_id;
        simulation::WorldTick expected_last_eval = 0;
    };

    struct LaterEvent {
        [[nodiscard]] bool operator()(const Event& lhs, const Event& rhs) const noexcept;
    };

    [[nodiscard]] core::Result<std::optional<Event>>
    predict_event(const ProcessInstance& process, ProcessModifiers modifiers,
                  simulation::WorldTick not_before) const;

    ProcessTemporalAggregationConfig config_;
    std::priority_queue<Event, std::vector<Event>, LaterEvent> events_;
    std::size_t admitted_process_count_ = 0;
    std::optional<simulation::WorldTick> last_world_time_;
};

} // namespace heartstead::processes
