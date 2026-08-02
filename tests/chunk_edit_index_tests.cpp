#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/chunks/chunk_edit_delta_codec.hpp"
#include "engine/world/streaming/chunk_streamer.hpp"
#include "engine/world/world_state.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using namespace heartstead;

class RecordingSaveSink final : public world::IChunkEditDeltaSink {
  public:
    [[nodiscard]] core::Status
    write_chunk_delta(const save::ChunkEditSaveRecord& chunk_delta) const override {
        records.push_back(chunk_delta);
        return core::Status::ok();
    }

    mutable std::vector<save::ChunkEditSaveRecord> records;
};

class RecordingReplicationSink final : public world::IChunkReplicationDeltaSink {
  public:
    [[nodiscard]] core::Status
    replicate_chunk_delta(const save::ChunkEditSaveRecord& chunk_delta) const override {
        records.push_back(chunk_delta);
        return core::Status::ok();
    }

    mutable std::vector<save::ChunkEditSaveRecord> records;
};

[[nodiscard]] world::VoxelCoord voxel_coord_for_index(std::size_t index) {
    constexpr auto edge = static_cast<std::size_t>(world::VoxelChunk::edge_length);
    return {
        static_cast<std::uint16_t>(index % edge),
        static_cast<std::uint16_t>((index / edge) % edge),
        static_cast<std::uint16_t>(index / (edge * edge)),
    };
}

void test_per_chunk_history_and_lazy_flat_view() {
    world::ChunkDatabase chunks;
    constexpr world::ChunkCoord first_coord{3, 0, 0};
    constexpr world::ChunkCoord second_coord{-2, 1, 4};
    constexpr world::VoxelCoord first_voxel{1, 2, 3};
    constexpr world::VoxelCoord second_voxel{4, 5, 6};

    assert(chunks.edits_for_chunk(first_coord).empty());
    assert(chunks.stats().edit_log_cache_rebuild_count == 0);
    assert(chunks.set(first_coord, first_voxel, world::VoxelCell{7, 0}));

    const auto first_edits = chunks.edits_for_chunk(first_coord);
    assert(first_edits.size() == 1);
    assert(first_edits.front().voxel_coord == first_voxel);
    assert(chunks.edits_for_chunk(second_coord).empty());
    assert(chunks.stats().edit_count == 1);
    assert(chunks.stats().edited_chunk_count == 1);
    assert(chunks.stats().edit_log_cache_rebuild_count == 0);

    assert(chunks.edit_log().size() == 1);
    assert(chunks.stats().edit_log_cache_rebuild_count == 1);
    assert(chunks.edit_log().size() == 1);
    assert(chunks.stats().edit_log_cache_rebuild_count == 1);

    assert(chunks.set(second_coord, second_voxel, world::VoxelCell{8, 0}));
    assert(chunks.edits_for_chunk(second_coord).size() == 1);
    assert(chunks.stats().edit_log_cache_rebuild_count == 1);
    assert(chunks.edit_log().size() == 2);
    assert(chunks.stats().edit_log_cache_rebuild_count == 2);

    assert(chunks.set(first_coord, first_voxel, world::VoxelCell::air()));
    assert(chunks.edits_for_chunk(first_coord).empty());
    assert(chunks.stats().edit_count == 1);
    assert(chunks.stats().edited_chunk_count == 1);
}

void test_publication_and_flush_ignore_unrelated_history() {
    constexpr std::size_t unrelated_edit_count = 4096;
    constexpr world::ChunkCoord unrelated_coord{20, 0, 0};
    constexpr world::ChunkCoord target_coord{4, 0, 0};
    constexpr world::VoxelCoord old_target_voxel{1, 1, 1};
    constexpr world::VoxelCoord replacement_target_voxel{2, 2, 2};

    world::WorldState state;
    std::vector<world::VoxelEditRecord> unrelated_edits;
    unrelated_edits.reserve(unrelated_edit_count);
    for (std::size_t index = 0; index < unrelated_edit_count; ++index) {
        unrelated_edits.push_back({
            unrelated_coord,
            voxel_coord_for_index(index),
            world::VoxelCell::air(),
            world::VoxelCell{7, 0},
        });
    }

    auto unrelated = world::ChunkDatabase::prepare_generated(world::VoxelChunk{unrelated_coord},
                                                             unrelated_edits);
    assert(unrelated);
    assert(state.chunks().insert_prepared_generated(std::move(unrelated).value()));
    assert(state.chunks().set(target_coord, old_target_voxel, world::VoxelCell{2, 0}));
    assert(state.chunks().edit_log().size() == unrelated_edit_count + 1);
    const auto rebuilds_before_publication = state.chunks().stats().edit_log_cache_rebuild_count;

    // Unloading intentionally retains the saved delta. Publishing a freshly generated residency
    // must replace only this coordinate's history, without flattening/copying unrelated chunks.
    assert(state.chunks().erase(target_coord));
    const std::vector<world::VoxelEditRecord> replacement_edits{{
        target_coord,
        replacement_target_voxel,
        world::VoxelCell::air(),
        world::VoxelCell{3, 0},
    }};
    auto replacement =
        world::ChunkDatabase::prepare_generated(world::VoxelChunk{target_coord}, replacement_edits);
    assert(replacement);
    assert(state.chunks().insert_prepared_generated(std::move(replacement).value()));

    assert(state.chunks().stats().edit_log_cache_rebuild_count == rebuilds_before_publication);
    assert(state.chunks().edits_for_chunk(unrelated_coord).size() == unrelated_edit_count);
    const auto target_edits = state.chunks().edits_for_chunk(target_coord);
    assert(target_edits.size() == 1);
    assert(target_edits.front().voxel_coord == replacement_target_voxel);
    assert(state.chunks().get(target_coord, old_target_voxel).value().is_air());
    assert(state.chunks().get(target_coord, replacement_target_voxel).value().type == 3);

    // Mutate the loaded delta, leaving the flat compatibility view invalid. Both narrow flush
    // paths must consume the target chunk's indexed history without rebuilding that view.
    assert(state.chunks().set(target_coord, replacement_target_voxel, world::VoxelCell{4, 0}));
    RecordingSaveSink save_sink;
    const std::vector<world::ChunkCoord> requested{target_coord};
    auto saved = world::ChunkStreamer::flush_save_deltas(state, requested, save_sink);
    assert(saved);
    assert(saved.value().written_count() == 1);
    assert(save_sink.records.size() == 1);
    assert(state.chunks().stats().edit_log_cache_rebuild_count == rebuilds_before_publication);

    auto saved_edits = world::ChunkEditDeltaTextCodec::decode(
        target_coord, save_sink.records.front().encoded_edit_delta);
    assert(saved_edits);
    assert(saved_edits.value().size() == 1);
    assert(saved_edits.value().front().chunk_coord == target_coord);
    assert(saved_edits.value().front().voxel_coord == replacement_target_voxel);
    assert(saved_edits.value().front().next.type == 4);

    RecordingReplicationSink replication_sink;
    auto replicated =
        world::ChunkStreamer::flush_replication_deltas(state, requested, replication_sink);
    assert(replicated);
    assert(replicated.value().replicated_count() == 1);
    assert(replication_sink.records.size() == 1);
    assert(state.chunks().stats().edit_log_cache_rebuild_count == rebuilds_before_publication);

    assert(state.chunks().edit_log().size() == unrelated_edit_count + 1);
    assert(state.chunks().stats().edit_log_cache_rebuild_count == rebuilds_before_publication + 1);
}

} // namespace

int main() {
    test_per_chunk_history_and_lazy_flat_view();
    test_publication_and_flush_ignore_unrelated_history();
    return 0;
}
