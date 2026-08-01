#include "engine/core/result.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/world/streaming/chunk_streamer.hpp"
#include "engine/world/world_state.hpp"

#include <cassert>
#include <cstddef>
#include <vector>

namespace {

namespace core = heartstead::core;
namespace save = heartstead::save;
namespace world = heartstead::world;

constexpr world::ChunkCoord test_chunk{4, -2, 7};
constexpr world::VoxelCoord first_voxel{1, 2, 3};
constexpr world::VoxelCoord second_voxel{4, 5, 6};

class RecordingSaveSink final : public world::IChunkEditDeltaSink {
  public:
    [[nodiscard]] core::Status
    write_chunk_delta(const save::ChunkEditSaveRecord& chunk_delta) const override {
        records.push_back(chunk_delta);
        return core::Status::ok();
    }

    mutable std::vector<save::ChunkEditSaveRecord> records;
};

class EditingSaveSink final : public world::IChunkEditDeltaSink {
  public:
    explicit EditingSaveSink(world::WorldState& state) noexcept : state_(&state) {}

    [[nodiscard]] core::Status
    write_chunk_delta(const save::ChunkEditSaveRecord& chunk_delta) const override {
        records.push_back(chunk_delta);
        if (!edited_) {
            edited_ = true;
            auto status = state_->chunks().set(test_chunk, second_voxel, world::VoxelCell{2, 0});
            if (!status) {
                return status;
            }
        }
        return core::Status::ok();
    }

    world::WorldState* state_ = nullptr;
    mutable bool edited_ = false;
    mutable std::vector<save::ChunkEditSaveRecord> records;
};

class ReloadingReplicationSink final : public world::IChunkReplicationDeltaSink {
  public:
    explicit ReloadingReplicationSink(world::WorldState& state) noexcept : state_(&state) {}

    [[nodiscard]] core::Status
    replicate_chunk_delta(const save::ChunkEditSaveRecord& chunk_delta) const override {
        records.push_back(chunk_delta);
        if (!reloaded_) {
            reloaded_ = true;
            assert(state_->chunks().erase(test_chunk));
            state_->chunks().clear_edit_log();
            auto status = state_->chunks().set(test_chunk, first_voxel, world::VoxelCell{3, 0});
            if (!status) {
                return status;
            }
        }
        return core::Status::ok();
    }

    world::WorldState* state_ = nullptr;
    mutable bool reloaded_ = false;
    mutable std::vector<save::ChunkEditSaveRecord> records;
};

void test_stage_transition_contract() {
    world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create(test_chunk);
    const auto initial_identity = chunk.identity();
    const auto initial_mesh = chunk.stage_ticket(world::ChunkStage::mesh);
    assert(initial_mesh.is_valid());
    assert(chunk.stages().record(world::ChunkStage::mesh).resident_is_current());

    const auto requested = chunk.request_stage(world::ChunkStage::mesh);
    assert(requested.identity == initial_identity);
    assert(requested.revision == initial_mesh.revision + 1);
    assert(chunk.stages().record(world::ChunkStage::mesh).state ==
           world::ChunkStageState::requested);
    assert(chunk.mark_stage_running(requested));
    assert(chunk.stages().record(world::ChunkStage::mesh).state == world::ChunkStageState::running);
    assert(chunk.mark_stage_ready(requested));
    const auto output_before = chunk.stages().record(world::ChunkStage::mesh).output_revision;
    assert(chunk.publish_stage(requested));
    const auto& published = chunk.stages().record(world::ChunkStage::mesh);
    assert(published.resident_is_current());
    assert(published.output_revision == output_before + 1);

    const auto obsolete = chunk.request_stage(world::ChunkStage::mesh);
    assert(chunk.mark_stage_running(obsolete));
    const auto current = chunk.request_stage(world::ChunkStage::mesh);
    assert(!chunk.stage_ticket_is_current(obsolete));
    assert(chunk.stage_ticket_is_current(current));
    assert(!chunk.mark_stage_ready(obsolete));
    assert(chunk.note_stage_stale(obsolete));
    assert(chunk.stages().record(world::ChunkStage::mesh).stale_results == 1);
    assert(chunk.stages().record(world::ChunkStage::mesh).state ==
           world::ChunkStageState::requested);

    assert(chunk.mark_stage_running(current));
    assert(chunk.note_stage_cancelled(current));
    assert(chunk.stages().record(world::ChunkStage::mesh).state ==
           world::ChunkStageState::cancelled);
    assert(chunk.stages().record(world::ChunkStage::mesh).cancelled_results == 1);
    const auto retry = chunk.ensure_stage_requested(world::ChunkStage::mesh);
    assert(retry.revision == current.revision + 1);

    const auto stats = chunks.stats();
    const auto& mesh_stats = stats.stage(world::ChunkStage::mesh);
    assert(mesh_stats.requested == 1);
    assert(mesh_stats.available_resident_outputs == 1);
    assert(mesh_stats.stale_results == 1);
    assert(mesh_stats.cancelled_results == 1);
}

void test_content_changes_invalidate_only_dependent_stages() {
    world::ChunkDatabase chunks;
    auto& chunk = chunks.get_or_create(test_chunk);
    const auto initial_content = chunk.content_revision();
    const auto initial_persistence = chunk.stage_ticket(world::ChunkStage::persistence);
    const auto initial_replication = chunk.stage_ticket(world::ChunkStage::replication);

    assert(chunk.set(first_voxel, world::VoxelCell{1, 0}));
    assert(chunk.content_revision() == initial_content + 1);
    assert(chunk.stages().record(world::ChunkStage::content).output_revision ==
           chunk.content_revision());
    assert(!chunk.stage_ticket_is_current(initial_persistence));
    assert(!chunk.stage_ticket_is_current(initial_replication));
    assert(chunk.dirty().contains(world::ChunkDirtyFlag::save));
    assert(chunk.dirty().contains(world::ChunkDirtyFlag::replication));

    const auto lighting = chunk.stage_ticket(world::ChunkStage::lighting);
    const auto mesh = chunk.stage_ticket(world::ChunkStage::mesh);
    const auto collision = chunk.stage_ticket(world::ChunkStage::collision);
    const auto persistence = chunk.stage_ticket(world::ChunkStage::persistence);
    const auto replication = chunk.stage_ticket(world::ChunkStage::replication);
    std::vector<std::uint8_t> light(world::VoxelChunk::total_cells, 0);
    light.front() = 12;
    auto applied = chunk.apply_derived_light(light);
    assert(applied);
    assert(applied.value() == 1);
    assert(chunk.stage_ticket_is_current(collision));
    assert(chunk.stage_ticket_is_current(lighting));
    assert(chunk.stage_ticket_is_current(persistence));
    assert(!chunk.stage_ticket_is_current(mesh));
    assert(!chunk.stage_ticket_is_current(replication));
    assert(chunk.dirty().contains(world::ChunkDirtyFlag::mesh));
    assert(chunk.dirty().contains(world::ChunkDirtyFlag::save));
}

void test_save_acknowledgement_rejects_reentrant_edit() {
    world::WorldState state;
    assert(state.chunks().set(test_chunk, first_voxel, world::VoxelCell{1, 0}));
    auto* chunk = state.chunks().find(test_chunk);
    assert(chunk != nullptr);
    const auto first_ticket = chunk->stage_ticket(world::ChunkStage::persistence);

    EditingSaveSink editing_sink(state);
    const std::vector<world::ChunkCoord> request{test_chunk};
    auto first_flush = world::ChunkStreamer::flush_save_deltas(state, request, editing_sink);
    assert(first_flush);
    assert(first_flush.value().written_chunks.empty());
    assert(first_flush.value().stale_chunks == request);
    assert(editing_sink.records.size() == 1);

    chunk = state.chunks().find(test_chunk);
    assert(chunk != nullptr);
    assert(chunk->identity() == first_ticket.identity);
    assert(!chunk->stage_ticket_is_current(first_ticket));
    assert(chunk->dirty().contains(world::ChunkDirtyFlag::save));
    assert(chunk->stages().record(world::ChunkStage::persistence).state ==
           world::ChunkStageState::requested);
    assert(chunk->stages().record(world::ChunkStage::persistence).stale_results == 1);

    RecordingSaveSink recording_sink;
    auto second_flush = world::ChunkStreamer::flush_save_deltas(state, request, recording_sink);
    assert(second_flush);
    assert(second_flush.value().written_chunks == request);
    assert(second_flush.value().stale_chunks.empty());
    assert(recording_sink.records.size() == 1);
    chunk = state.chunks().find(test_chunk);
    assert(chunk != nullptr);
    assert(!chunk->dirty().contains(world::ChunkDirtyFlag::save));
    assert(chunk->stages().record(world::ChunkStage::persistence).resident_is_current());
}

void test_replication_acknowledgement_rejects_reloaded_generation() {
    world::WorldState state;
    assert(state.chunks().set(test_chunk, first_voxel, world::VoxelCell{1, 0}));
    const auto old_identity = state.chunks().find(test_chunk)->identity();

    ReloadingReplicationSink sink(state);
    const std::vector<world::ChunkCoord> request{test_chunk};
    auto flush = world::ChunkStreamer::flush_replication_deltas(state, request, sink);
    assert(flush);
    assert(flush.value().replicated_chunks.empty());
    assert(flush.value().stale_chunks == request);
    assert(sink.records.size() == 1);

    const auto* replacement = state.chunks().find(test_chunk);
    assert(replacement != nullptr);
    assert(replacement->identity() != old_identity);
    assert(replacement->dirty().contains(world::ChunkDirtyFlag::replication));
    assert(replacement->stages().record(world::ChunkStage::replication).state ==
           world::ChunkStageState::requested);
}

} // namespace

int main() {
    test_stage_transition_contract();
    test_content_changes_invalidate_only_dependent_stages();
    test_save_acknowledgement_rejects_reentrant_edit();
    test_replication_acknowledgement_rejects_reloaded_generation();
    return 0;
}
