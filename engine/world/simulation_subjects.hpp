#pragma once

#include "engine/core/result.hpp"
#include "engine/simulation/simulation_lod.hpp"

#include <cstdint>
#include <vector>

namespace heartstead::world {

class WorldState;

struct WorldSimulationSubjectOptions {
    bool include_entities = true;
    bool include_build_pieces = true;
    bool include_assemblies = true;
    bool include_processes = true;
    bool include_networks = true;
    bool include_chunk_regions = true;
    simulation::WorldTick last_update_time_ms = 0;
};

struct WorldSimulationFramePlanOptions {
    WorldSimulationSubjectOptions subject_options;
    std::vector<simulation::SimulationViewer> viewers;
    simulation::SimulationLodPolicy policy;
    simulation::SimulationTickBudget tick_budget;
    simulation::WorldTick now_ms = 0;
};

[[nodiscard]] core::Result<std::vector<simulation::SimulationSubject>>
derive_simulation_subjects(const WorldState& state, WorldSimulationSubjectOptions options = {});

[[nodiscard]] core::Result<simulation::SimulationFramePlan>
plan_world_simulation_frame(const WorldState& state,
                            const WorldSimulationFramePlanOptions& options);

[[nodiscard]] core::Result<simulation::BudgetedSimulationFramePlan>
plan_budgeted_world_simulation_frame(const WorldState& state,
                                     const WorldSimulationFramePlanOptions& options);

} // namespace heartstead::world
