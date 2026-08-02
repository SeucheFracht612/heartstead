#include "engine/world/simulation_subjects.hpp"

#include "engine/core/hash.hpp"
#include "engine/entities/entity.hpp"
#include "engine/math/vector.hpp"
#include "engine/processes/process.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace heartstead::world {

namespace {

[[nodiscard]] simulation::SimulationCoord
coord_from_position(const WorldPosition& position) noexcept {
    return position.anchor;
}

[[nodiscard]] simulation::SimulationCoord
coord_from_network_coord(networks::NetworkCoord coord) noexcept {
    return {
        coord.x,
        coord.y,
        coord.z,
    };
}

[[nodiscard]] core::Result<simulation::SimulationCoord> coord_from_chunk_coord(ChunkCoord coord) {
    return chunk_local_to_block(coord, {});
}

[[nodiscard]] std::uint64_t coordinate_bits(std::int64_t value) noexcept {
    return static_cast<std::uint64_t>(value) ^ 0x8000000000000000ULL;
}

[[nodiscard]] core::RuntimeHandle network_runtime_handle(networks::NetworkKind kind,
                                                         networks::NetworkNodeId node_id) noexcept {
    core::StableHash64 hasher;
    hasher.add_string("simulation_network_subject");
    hasher.add_u64_le(static_cast<std::uint64_t>(kind));
    hasher.add_u64_le(node_id.value());
    return core::RuntimeHandle::from_value(hasher.nonzero_value());
}

[[nodiscard]] core::RuntimeHandle chunk_runtime_handle(ChunkCoord coord) noexcept {
    core::StableHash64 hasher;
    hasher.add_string("simulation_chunk_region_subject");
    hasher.add_u64_le(coordinate_bits(coord.x));
    hasher.add_u64_le(coordinate_bits(coord.y));
    hasher.add_u64_le(coordinate_bits(coord.z));
    return core::RuntimeHandle::from_value(hasher.nonzero_value());
}

[[nodiscard]] std::uint64_t subject_sort_key(const simulation::SimulationSubject& subject) {
    if (subject.process_id.is_valid()) {
        return subject.process_id.value();
    }
    if (subject.save_id.is_valid()) {
        return subject.save_id.value();
    }
    return subject.runtime_handle.value();
}

void sort_subjects(std::vector<simulation::SimulationSubject>& subjects) {
    std::ranges::sort(subjects, [](const auto& lhs, const auto& rhs) {
        if (lhs.kind != rhs.kind) {
            return static_cast<int>(lhs.kind) < static_cast<int>(rhs.kind);
        }
        if (subject_sort_key(lhs) != subject_sort_key(rhs)) {
            return subject_sort_key(lhs) < subject_sort_key(rhs);
        }
        if (lhs.prototype_id != rhs.prototype_id) {
            return lhs.prototype_id.value() < rhs.prototype_id.value();
        }
        return lhs.label < rhs.label;
    });
}

[[nodiscard]] std::optional<simulation::SimulationCoord>
coord_for_saved_object(const WorldState& state, core::SaveId id) {
    if (const auto* build_piece = state.build_objects().find(id)) {
        return coord_from_position(build_piece->transform.position);
    }
    if (const auto* entity = state.entities().find_by_save_id(id)) {
        return coord_from_position(entity->transform.position);
    }
    if (const auto* cargo = state.cargo().find(id)) {
        return coord_from_position(cargo->position);
    }
    if (const auto* assembly = state.assemblies().find(id)) {
        if (const auto* root = state.build_objects().find(assembly->root_build_piece_id)) {
            return coord_from_position(root->transform.position);
        }
    }
    return std::nullopt;
}

} // namespace

core::Result<std::vector<simulation::SimulationSubject>>
derive_simulation_subjects(const WorldState& state, WorldSimulationSubjectOptions options) {
    std::vector<simulation::SimulationSubject> subjects;

    if (options.include_entities) {
        const auto entity_records = state.entities().records();
        subjects.reserve(subjects.size() + entity_records.size());
        for (const auto* entity : entity_records) {
            simulation::SimulationSubject subject;
            subject.save_id = entity->save_id;
            subject.runtime_handle = entity->runtime_handle;
            subject.prototype_id = entity->prototype_id;
            subject.kind = simulation::SimulationSubjectKind::entity;
            subject.coord = coord_from_position(entity->transform.position);
            subject.last_update_time_ms = options.last_update_time_ms;
            subject.persistent = entity->persistent;
            subject.sleeping = entity->sleeping;
            subject.label = std::string(entities::entity_kind_name(entity->kind));
            subjects.push_back(std::move(subject));
        }
    }

    if (options.include_build_pieces) {
        const auto build_pieces = state.build_objects().records();
        subjects.reserve(subjects.size() + build_pieces.size());
        for (const auto* build_piece : build_pieces) {
            simulation::SimulationSubject subject;
            subject.save_id = build_piece->object_id;
            subject.prototype_id = build_piece->prototype_id;
            subject.kind = simulation::SimulationSubjectKind::build_piece;
            subject.coord = coord_from_position(build_piece->transform.position);
            subject.last_update_time_ms = options.last_update_time_ms;
            subject.persistent = true;
            subject.label = "build_piece";
            subjects.push_back(std::move(subject));
        }
    }

    if (options.include_assemblies) {
        const auto assembly_records = state.assemblies().records();
        subjects.reserve(subjects.size() + assembly_records.size());
        for (const auto* assembly : assembly_records) {
            const auto coord = coord_for_saved_object(state, assembly->assembly_id);
            if (!coord.has_value()) {
                continue;
            }

            simulation::SimulationSubject subject;
            subject.save_id = assembly->assembly_id;
            subject.prototype_id = assembly->prototype_id;
            subject.kind = simulation::SimulationSubjectKind::assembly;
            subject.coord = coord.value();
            subject.last_update_time_ms = options.last_update_time_ms;
            subject.persistent = true;
            subject.sleeping = !assembly->operating;
            subject.label = "assembly";
            subjects.push_back(std::move(subject));
        }
    }

    if (options.include_processes) {
        const auto process_records = state.processes().records();
        subjects.reserve(subjects.size() + process_records.size());
        for (const auto* process : process_records) {
            const auto coord = coord_for_saved_object(state, process->owner_id);
            if (!coord.has_value()) {
                continue;
            }

            simulation::SimulationSubject subject;
            subject.save_id = process->owner_id;
            subject.process_id = process->process_id;
            subject.prototype_id = process->prototype_id;
            subject.kind = simulation::SimulationSubjectKind::process_owner;
            subject.coord = coord.value();
            subject.last_update_time_ms = process->last_update_time_ms;
            subject.persistent = true;
            subject.sleeping = process->state != processes::ProcessState::running;
            subject.label = "process:" + process->process_id.to_string();
            subjects.push_back(std::move(subject));
        }
    }

    if (options.include_networks) {
        const auto network_records = state.networks().records();
        for (const auto* network : network_records) {
            const auto nodes = network->nodes();
            subjects.reserve(subjects.size() + nodes.size());
            for (const auto* node : nodes) {
                simulation::SimulationSubject subject;
                subject.runtime_handle = network_runtime_handle(network->kind(), node->id);
                subject.kind = simulation::SimulationSubjectKind::network;
                subject.coord = coord_from_network_coord(node->coord);
                subject.last_update_time_ms = options.last_update_time_ms;
                subject.persistent = false;
                subject.sleeping = !network->is_dirty();
                subject.label =
                    "network:" + std::string(networks::network_kind_name(network->kind()));
                subjects.push_back(std::move(subject));
            }
        }
    }

    if (options.include_chunk_regions) {
        const auto chunk_records = state.chunks().records();
        subjects.reserve(subjects.size() + chunk_records.size());
        for (const auto* chunk : chunk_records) {
            const auto coord = chunk->coord();
            auto simulation_coord = coord_from_chunk_coord(coord);
            if (!simulation_coord) {
                return core::Result<std::vector<simulation::SimulationSubject>>::failure(
                    simulation_coord.error().code, simulation_coord.error().message);
            }

            simulation::SimulationSubject subject;
            subject.runtime_handle = chunk_runtime_handle(coord);
            subject.kind = simulation::SimulationSubjectKind::chunk_region;
            subject.coord = simulation_coord.value();
            subject.last_update_time_ms = options.last_update_time_ms;
            subject.persistent = false;
            subject.sleeping = chunk->dirty().bits() == 0;
            subject.label = "chunk_region";
            subjects.push_back(std::move(subject));
        }
    }

    sort_subjects(subjects);
    return core::Result<std::vector<simulation::SimulationSubject>>::success(std::move(subjects));
}

core::Result<simulation::SimulationFramePlan>
plan_world_simulation_frame(const WorldState& state,
                            const WorldSimulationFramePlanOptions& options) {
    auto subjects = derive_simulation_subjects(state, options.subject_options);
    if (!subjects) {
        return core::Result<simulation::SimulationFramePlan>::failure(subjects.error().code,
                                                                      subjects.error().message);
    }

    return simulation::SimulationLodPlanner::plan_frame(subjects.value(), options.viewers,
                                                        options.policy, options.now_ms);
}

core::Result<simulation::BudgetedSimulationFramePlan>
plan_budgeted_world_simulation_frame(const WorldState& state,
                                     const WorldSimulationFramePlanOptions& options) {
    auto subjects = derive_simulation_subjects(state, options.subject_options);
    if (!subjects) {
        return core::Result<simulation::BudgetedSimulationFramePlan>::failure(
            subjects.error().code, subjects.error().message);
    }

    return simulation::SimulationLodPlanner::plan_budgeted_frame(
        subjects.value(), options.viewers, options.policy, options.tick_budget, options.now_ms);
}

} // namespace heartstead::world
