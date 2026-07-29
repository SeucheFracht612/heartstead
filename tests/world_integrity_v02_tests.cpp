#include "engine/modding/mod_fingerprint.hpp"
#include "engine/net/command_payload.hpp"
#include "engine/net/server_command.hpp"
#include "engine/processes/process.hpp"
#include "engine/save/save_compatibility.hpp"
#include "engine/save/save_snapshot.hpp"
#include "engine/simulation/fire.hpp"
#include "engine/workpieces/workpiece_codec.hpp"
#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/chunks/chunk_edit_delta_codec.hpp"
#include "engine/world/replication_delta.hpp"
#include "engine/world/streaming/chunk_streamer.hpp"
#include "engine/world/voxel_change.hpp"
#include "engine/world/voxels/voxel_palette.hpp"
#include "engine/world/world_commands.hpp"
#include "engine/world/world_snapshot.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TerrainFixture {
    heartstead::world::VoxelPalette palette;
    heartstead::world::RegionGraph regions;
    heartstead::world::TerrainGenerationConfig generation;

    TerrainFixture() {
        auto clay_id = heartstead::core::PrototypeId::parse("base:voxels/clay");
        auto tree_id = heartstead::core::PrototypeId::parse("base:features/integrity_tree");
        assert(clay_id);
        assert(tree_id);

        heartstead::world::VoxelDefinition clay;
        clay.type = 1;
        clay.prototype_id = clay_id.value();
        clay.display_name = "Clay";
        clay.terrain_material = "clay";
        clay.mining_tool = "shovel";
        assert(palette.add(std::move(clay)));

        heartstead::world::RegionDescriptor region;
        region.id = "integrity_test_region";
        region.age = "test_age";
        region.biome_cluster = "test_biome";
        region.resource_rules.push_back({clay_id.value(), "terrain", 1.0});
        region.resource_rules.push_back({tree_id.value(), "large_static_object", 1.0});
        assert(regions.add_region(std::move(region)));

        generation.world_seed = 42;
        generation.region_id = "integrity_test_region";
        generation.base_surface_y = 8;
    }
};

struct TestDeltaSource final : heartstead::world::IChunkEditDeltaSource {
    std::optional<heartstead::save::ChunkEditSaveRecord> record;
    bool fail_read = false;

    [[nodiscard]] heartstead::core::Result<std::optional<heartstead::save::ChunkEditSaveRecord>>
    read_chunk_delta(heartstead::world::ChunkCoord coord) const override {
        if (fail_read) {
            return heartstead::core::Result<std::optional<heartstead::save::ChunkEditSaveRecord>>::
                failure("test.delta_read_failed", "injected delta read failure");
        }
        if (!record.has_value() || record->coord != coord) {
            return heartstead::core::Result<
                std::optional<heartstead::save::ChunkEditSaveRecord>>::success(std::nullopt);
        }
        return heartstead::core::Result<
            std::optional<heartstead::save::ChunkEditSaveRecord>>::success(record);
    }
};

[[nodiscard]] heartstead::save::ChunkEditSaveRecord
encoded_delta(heartstead::world::VoxelEditRecord& edit) {
    const std::vector<const heartstead::world::VoxelEditRecord*> edits{&edit};
    return {edit.chunk_coord,
            heartstead::world::ChunkEditDeltaTextCodec::encode(edit.chunk_coord, edits)};
}

void test_failed_stream_load_does_not_publish_chunk() {
    TerrainFixture fixture;
    heartstead::world::WorldState state;
    TestDeltaSource source;
    source.fail_read = true;

    const heartstead::world::ChunkCoord read_failure_coord{4, 0, 0};
    auto read_failure = heartstead::world::ChunkStreamer::load_chunk(
        state, read_failure_coord, fixture.generation, fixture.regions, fixture.palette, &source);
    assert(!read_failure);
    assert(!state.chunks().contains(read_failure_coord));

    source.fail_read = false;
    const heartstead::world::ChunkCoord decode_failure_coord{5, 0, 0};
    source.record =
        heartstead::save::ChunkEditSaveRecord{decode_failure_coord, "not-a-heartstead-chunk-delta"};
    auto decode_failure = heartstead::world::ChunkStreamer::load_chunk(
        state, decode_failure_coord, fixture.generation, fixture.regions, fixture.palette, &source);
    assert(!decode_failure);
    assert(!state.chunks().contains(decode_failure_coord));

    const heartstead::world::ChunkCoord bounds_failure_coord{6, 0, 0};
    source.record = heartstead::save::ChunkEditSaveRecord{
        bounds_failure_coord,
        "heartstead.chunk_edit_delta.v1\ncoord=6|0|0\nedit=32|0|0|0|0|1|0\nend\n"};
    auto bounds_failure = heartstead::world::ChunkStreamer::load_chunk(
        state, bounds_failure_coord, fixture.generation, fixture.regions, fixture.palette, &source);
    assert(!bounds_failure);
    assert(bounds_failure.error().code == "world_snapshot.chunk_delta_voxel_out_of_bounds");
    assert(!state.chunks().contains(bounds_failure_coord));
}

void test_worldgen_preserves_i64_height_and_rejects_unrepresentable_chunks() {
    TerrainFixture fixture;
    constexpr std::int64_t far_height = std::int64_t{1} << 50;
    fixture.generation.base_surface_y = far_height;
    assert(heartstead::world::DeterministicTerrainGenerator::surface_height_at(
               fixture.generation, -9, 17) == far_height);

    const heartstead::world::ChunkCoord impossible{std::numeric_limits<std::int64_t>::max(), 0, 0};
    auto generated = heartstead::world::DeterministicTerrainGenerator::generate_chunk(
        impossible, fixture.generation, fixture.regions, fixture.palette);
    assert(!generated);
    assert(generated.error().code == "terrain_generator.chunk_coord_overflow");

    fixture.generation.base_surface_y = 0;
    fixture.generation.surface_variation = std::numeric_limits<std::uint16_t>::max();
    assert(heartstead::world::DeterministicTerrainGenerator::surface_height_at(fixture.generation,
                                                                               12, 34) == -7'936);
    bool far_coordinate_changes_height = false;
    for (std::int64_t x = 0; x < 64; ++x) {
        const auto near_height =
            heartstead::world::DeterministicTerrainGenerator::surface_height_at(fixture.generation,
                                                                                x, 0);
        const auto far_height_at_x =
            heartstead::world::DeterministicTerrainGenerator::surface_height_at(
                fixture.generation, x, std::numeric_limits<std::int64_t>::min());
        far_coordinate_changes_height |= near_height != far_height_at_x;
    }
    assert(far_coordinate_changes_height);
}

void test_worldgen_features_only_belong_to_their_surface_chunk() {
    TerrainFixture fixture;

    auto surface_chunk =
        heartstead::world::DeterministicTerrainGenerator::generate_chunk_with_features(
            {0, 0, 0}, fixture.generation, fixture.regions, fixture.palette);
    assert(surface_chunk);
    assert(!surface_chunk.value().features.empty());
    assert((surface_chunk.value().features.front().position ==
            heartstead::world::BlockCoord{0, 9, 0}));
    assert(surface_chunk.value().features.front().deterministic_seed ==
           11'419'484'320'585'080'512ULL);
    assert(std::ranges::all_of(surface_chunk.value().features, [](const auto& feature) {
        return heartstead::world::chunk_coord_for_block(feature.position) ==
               heartstead::world::ChunkCoord{0, 0, 0};
    }));

    const auto far_chunk_z =
        heartstead::world::chunk_axis_for_block(std::numeric_limits<std::int64_t>::min());
    auto far_surface_chunk =
        heartstead::world::DeterministicTerrainGenerator::generate_chunk_with_features(
            {0, 0, far_chunk_z}, fixture.generation, fixture.regions, fixture.palette);
    assert(far_surface_chunk);
    assert(!far_surface_chunk.value().features.empty());
    assert(far_surface_chunk.value().features.front().deterministic_seed !=
           surface_chunk.value().features.front().deterministic_seed);

    auto vertically_repeated =
        heartstead::world::DeterministicTerrainGenerator::generate_chunk_with_features(
            {0, 1, 0}, fixture.generation, fixture.regions, fixture.palette);
    assert(vertically_repeated);
    assert(vertically_repeated.value().features.empty());

    fixture.generation.base_surface_y = std::numeric_limits<std::int64_t>::max();
    const auto top_chunk_y =
        heartstead::world::chunk_axis_for_block(std::numeric_limits<std::int64_t>::max());
    auto unrepresentable_feature =
        heartstead::world::DeterministicTerrainGenerator::generate_chunk_with_features(
            {0, top_chunk_y, 0}, fixture.generation, fixture.regions, fixture.palette);
    assert(unrepresentable_feature);
    assert(unrepresentable_feature.value().features.empty());
}

void test_chunk_interest_planning_is_bounded() {
    heartstead::world::WorldState state;
    heartstead::world::ChunkStreamInterestPolicy policy;
    policy.load_horizontal_radius_chunks = static_cast<std::uint16_t>(
        heartstead::world::ChunkStreamInterestPolicy::max_load_radius_chunks + 1U);
    policy.retain_horizontal_radius_chunks = policy.load_horizontal_radius_chunks;
    auto plan = heartstead::world::ChunkStreamer::plan_interest(state, {}, policy);
    assert(!plan);
    assert(plan.error().code == "chunk_stream.load_radius_too_large");
}

void test_saved_edit_batch_validation_is_atomic() {
    heartstead::world::ChunkDatabase chunks;
    const heartstead::world::ChunkCoord coord{7, 0, 0};
    chunks.get_or_create(coord).clear_all_dirty();

    const std::vector<heartstead::world::VoxelEditRecord> edits{
        {coord, {1, 2, 3}, heartstead::world::VoxelCell::air(), {2, 0}},
        {coord,
         {heartstead::world::VoxelChunk::edge_length, 0, 0},
         heartstead::world::VoxelCell::air(),
         {3, 0}},
    };

    auto status = chunks.apply_saved_edits(edits);
    assert(!status);
    assert(status.error().code == "chunk_database.saved_edit_coord_out_of_bounds");
    auto unchanged = chunks.get(coord, {1, 2, 3});
    assert(unchanged);
    assert(unchanged.value().is_air());
    assert(chunks.edit_log().empty());

    const heartstead::world::ChunkCoord invalid_chain_coord{8, 0, 0};
    const std::vector<heartstead::world::VoxelEditRecord> invalid_chain{
        {invalid_chain_coord, {1, 1, 1}, heartstead::world::VoxelCell::air(), {2, 0}},
        {invalid_chain_coord, {1, 1, 1}, {3, 0}, {4, 0}},
    };
    status = chunks.apply_saved_edits(invalid_chain);
    assert(!status);
    assert(status.error().code == "chunk_database.saved_edit_chain_mismatch");
    assert(!chunks.contains(invalid_chain_coord));
    assert(chunks.edit_log().empty());

    const heartstead::world::ChunkCoord incompatible_base_coord{9, 0, 0};
    auto& incompatible_base = chunks.get_or_create(incompatible_base_coord);
    assert(incompatible_base.apply_saved_cell({2, 2, 2}, {7, 0}));
    incompatible_base.clear_all_dirty();
    const std::vector<heartstead::world::VoxelEditRecord> incompatible_base_edit{
        {incompatible_base_coord, {2, 2, 2}, heartstead::world::VoxelCell::air(), {8, 0}},
    };
    status = chunks.apply_saved_edits(incompatible_base_edit);
    assert(!status);
    assert(status.error().code == "chunk_database.saved_edit_base_mismatch");
    auto preserved = chunks.get(incompatible_base_coord, {2, 2, 2});
    assert(preserved && preserved.value() == heartstead::world::VoxelCell(7, 0));
    assert(chunks.edit_log().empty());
}

void test_saved_edits_ignore_derived_light_when_matching_the_baseline() {
    using namespace heartstead;

    constexpr world::ChunkCoord coord{10, 0, 0};
    constexpr world::VoxelCoord voxel{4, 5, 6};
    constexpr world::VoxelCell sunlit_air{0, 255, 0, 0};
    constexpr world::VoxelCell placed_block{7, 0, 3, 11};
    const std::vector<world::VoxelEditRecord> edits{
        {coord, voxel, sunlit_air, placed_block},
    };

    world::ChunkDatabase resident;
    resident.get_or_create(coord).clear_all_dirty();
    assert(resident.apply_saved_edits(edits));
    auto restored = resident.get(coord, voxel);
    assert(restored && restored.value() == placed_block);

    std::vector<std::uint8_t> relit(world::VoxelChunk::total_cells, 19);
    assert(resident.find(coord)->apply_derived_light(relit));
    assert(resident.apply_saved_edits(edits));
    restored = resident.get(coord, voxel);
    assert(restored && restored.value() == placed_block);

    world::VoxelChunk generated(coord);
    world::ChunkDatabase streamed;
    assert(streamed.insert_generated_with_saved_edits(std::move(generated), edits));
    restored = streamed.get(coord, voxel);
    assert(restored && restored.value() == placed_block);
}

void test_chunk_mutations_preflight_and_compact_edit_history() {
    using namespace heartstead;

    world::ChunkDatabase chunks;
    const world::ChunkCoord coord{9, 0, 0};
    auto invalid = chunks.set(coord, {world::VoxelChunk::edge_length, 0, 0}, {1, 0});
    assert(!invalid);
    assert(!chunks.contains(coord));
    assert(chunks.edit_log().empty());

    assert(chunks.set(coord, {1, 1, 1}, world::VoxelCell::air()));
    assert(!chunks.contains(coord));

    for (std::uint16_t edit = 0; edit < 1'000; ++edit) {
        assert(chunks.set(coord, {1, 1, 1}, {static_cast<std::uint16_t>(1 + edit % 2), 0}));
    }
    assert(chunks.edit_log().size() == 1);
    assert(chunks.edit_log().front().previous == world::VoxelCell::air());
    assert(chunks.edit_log().front().next == world::VoxelCell(2, 0));
    assert(chunks.set(coord, {1, 1, 1}, world::VoxelCell::air()));
    assert(chunks.edit_log().empty());
    assert(chunks.find(coord)->dirty().contains(world::ChunkDirtyFlag::save));
    assert(chunks.find(coord)->dirty().contains(world::ChunkDirtyFlag::replication));

    const world::ChunkCoord restored_coord{10, 0, 0};
    world::ChunkDatabase restored_chunks;
    std::vector<world::VoxelEditRecord> long_chain;
    auto previous = world::VoxelCell::air();
    for (std::uint16_t edit = 0; edit < 1'000; ++edit) {
        const world::VoxelCell next{static_cast<std::uint16_t>(1 + edit % 2), 0};
        long_chain.push_back({restored_coord, {2, 2, 2}, previous, next});
        previous = next;
    }
    assert(restored_chunks.apply_saved_edits(long_chain));
    assert(restored_chunks.edit_log().size() == 1);
    assert(restored_chunks.edit_log().front().chunk_coord == restored_coord);
    assert(restored_chunks.edit_log().front().previous == world::VoxelCell::air());
    assert(restored_chunks.edit_log().front().next == previous);

    const std::vector<world::VoxelEditRecord> replacement_delta{
        {restored_coord, {3, 3, 3}, world::VoxelCell::air(), {5, 0}},
    };
    assert(restored_chunks.apply_saved_edits(replacement_delta));
    auto removed_edit = restored_chunks.get(restored_coord, {2, 2, 2});
    auto replacement_edit = restored_chunks.get(restored_coord, {3, 3, 3});
    assert(removed_edit && removed_edit.value().is_air());
    assert(replacement_edit && replacement_edit.value() == world::VoxelCell(5, 0));
    assert(restored_chunks.edit_log().size() == 1);
    assert(restored_chunks.edit_log().front().voxel_coord == (world::VoxelCoord{3, 3, 3}));

    world::VoxelChunk incompatible_base({11, 0, 0});
    incompatible_base.fill({7, 0});
    const std::vector<world::VoxelEditRecord> incompatible_delta{
        {{11, 0, 0}, {3, 3, 3}, world::VoxelCell::air(), {8, 0}},
    };
    auto rejected = restored_chunks.insert_generated_with_saved_edits(std::move(incompatible_base),
                                                                      incompatible_delta);
    assert(!rejected);
    assert(rejected.error().code == "chunk_database.saved_edit_base_mismatch");
    assert(!restored_chunks.contains({11, 0, 0}));
}

void test_chunk_residency_changes_invalidate_shared_seams() {
    using namespace heartstead;

    world::ChunkDatabase chunks;
    dirty::DirtyRegionTracker dirty_regions;
    std::vector<world::VoxelCell> solid_cells(world::VoxelChunk::total_cells,
                                              world::VoxelCell{1, 0});
    world::VoxelChunk left({0, 0, 0});
    assert(left.load_generated_cells(solid_cells));
    assert(chunks.insert_generated(std::move(left), dirty_regions));

    chunks.clear_all_dirty();
    dirty_regions.clear();
    world::VoxelChunk right({1, 0, 0});
    assert(right.load_generated_cells(std::move(solid_cells)));
    assert(chunks.insert_generated(std::move(right), dirty_regions));

    const auto* remeshed_left = chunks.find({0, 0, 0});
    assert(remeshed_left != nullptr);
    assert(remeshed_left->dirty().contains(world::ChunkDirtyFlag::mesh));
    assert(remeshed_left->dirty().contains(world::ChunkDirtyFlag::collision));
    assert(remeshed_left->dirty().contains(world::ChunkDirtyFlag::lighting));
    const auto mesh_regions = dirty_regions.consume_kind(dirty::DirtyRegionKind::chunk_mesh);
    assert(mesh_regions.size() == 1);
    assert((mesh_regions.front().bounds.min == dirty::DirtyRegionCoord{0, 0, 0}));
    assert((mesh_regions.front().bounds.max == dirty::DirtyRegionCoord{1, 0, 0}));

    chunks.clear_all_dirty();
    assert(chunks.erase({1, 0, 0}));
    assert(chunks.find({0, 0, 0})->dirty().contains(world::ChunkDirtyFlag::mesh));
}

void test_stream_reload_preserves_one_canonical_delta() {
    TerrainFixture fixture;
    heartstead::world::WorldState state;
    heartstead::world::VoxelEditRecord saved_edit{{8, 0, 0}, {1, 2, 3}, {1, 0}, {4, 0}};
    TestDeltaSource source;
    source.record = encoded_delta(saved_edit);

    auto first_load = heartstead::world::ChunkStreamer::load_chunk(
        state, saved_edit.chunk_coord, fixture.generation, fixture.regions, fixture.palette,
        &source);
    assert(first_load);
    assert(state.chunks().edit_log().size() == 1);

    const std::vector<heartstead::world::ChunkCoord> evict{saved_edit.chunk_coord};
    auto first_eviction = heartstead::world::ChunkStreamer::evict_chunks(state, evict);
    assert(first_eviction.evicted_count() == 1);

    auto second_load = heartstead::world::ChunkStreamer::load_chunk(
        state, saved_edit.chunk_coord, fixture.generation, fixture.regions, fixture.palette,
        &source);
    assert(second_load);
    assert(state.chunks().edit_log().size() == 1);

    assert(state.chunks().set(saved_edit.chunk_coord, {2, 2, 3}, {5, 0}));
    assert(state.chunks().edit_log().size() == 2);
}

void test_world_set_voxel_rejects_unloaded_chunk() {
    heartstead::world::WorldState state;
    heartstead::net::ServerCommandDispatcher dispatcher;
    assert(heartstead::world::WorldCommandRegistry::register_engine_commands(dispatcher));

    heartstead::net::CommandPayload payload;
    assert(payload.set("chunk", "9|0|0"));
    assert(payload.set("voxel", "1|2|3"));
    assert(payload.set("prototype", "test:voxels/command"));

    heartstead::net::CommandEnvelope command;
    command.sequence = 1;
    command.sender = heartstead::core::NetId::from_value(2);
    command.type = "world.set_voxel";
    command.payload = heartstead::net::CommandPayloadTextCodec::encode(payload);

    heartstead::net::CommandExecutionContext context;
    context.executor_role = heartstead::net::CommandExecutorRole::authoritative_server;
    context.world_state = &state;
    heartstead::world::VoxelPalette palette;
    heartstead::world::VoxelDefinition definition;
    definition.type = 6;
    definition.prototype_id = heartstead::core::PrototypeId::parse("test:voxels/command").value();
    definition.display_name = "Command";
    definition.terrain_material = "test";
    definition.mining_tool = "none";
    assert(palette.add(std::move(definition)));
    context.voxel_palette = &palette;

    auto rejected = dispatcher.dispatch(command, context);
    assert(!rejected);
    assert(rejected.error().code == "world_command.chunk_not_loaded");
    assert(!state.chunks().contains({9, 0, 0}));

    heartstead::world::VoxelChunk loaded({9, 0, 0});
    assert(state.chunks().insert_generated(std::move(loaded)));
    auto accepted = dispatcher.dispatch(command, context);
    assert(accepted);
    assert(accepted.value().events.size() == 1);
    assert(accepted.value().events.front().type == heartstead::world::voxel_changed_event_type);
    auto change =
        heartstead::world::VoxelChangeTextCodec::decode(accepted.value().events.front().message);
    assert(change);
    assert((change.value().position == heartstead::world::BlockCoord{289, 2, 3}));
    assert(change.value().previous == heartstead::world::VoxelCell::air());
    assert(change.value().current.type == 6);

    heartstead::net::ReplicationBatch batch;
    batch.command_sequence = command.sequence;
    batch.command_type = command.type;
    batch.events = accepted.value().events;
    auto delta = heartstead::world::materialize_replication_delta(state, batch);
    heartstead::world::WorldState client_state;
    auto applied = heartstead::world::apply_replication_delta(client_state, delta);
    assert(applied);
    assert(applied.value().voxel_edits_applied == 1);
    auto replicated = client_state.chunks().get({9, 0, 0}, {1, 2, 3});
    assert(replicated);
    assert(replicated.value().type == 6);
}

void test_process_advance_all_is_atomic() {
    auto process_prototype = heartstead::core::PrototypeId::parse("base:processes/test");
    assert(process_prototype);

    heartstead::world::ProcessDatabase processes;
    for (std::uint64_t value = 1; value <= 2; ++value) {
        auto instance = heartstead::processes::ProcessRuntime::create(
            heartstead::core::ProcessId::from_value(value),
            heartstead::core::SaveId::from_value(100 + value), process_prototype.value(), 0,
            10'000);
        assert(instance);
        assert(processes.insert(std::move(instance).value()));
    }

    std::size_t resolution_count = 0;
    auto advanced = processes.advance_all(
        1'000, [&resolution_count](const heartstead::processes::ProcessInstance&) {
            ++resolution_count;
            if (resolution_count == 2) {
                return heartstead::core::Result<heartstead::processes::ProcessModifiers>::failure(
                    "test.modifier_failed", "injected modifier failure");
            }
            return heartstead::core::Result<heartstead::processes::ProcessModifiers>::success({});
        });
    assert(!advanced);
    assert(resolution_count == 2);
    for (const auto* process : processes.records()) {
        assert(process->last_update_time_ms == 0);
        assert(process->accumulated_effective_work_ms == 0);
        assert(process->state == heartstead::processes::ProcessState::running);
    }
}

void test_snapshot_allocator_recovery_is_complete_and_overflow_safe() {
    using namespace heartstead;

    save::SaveSnapshot snapshot;
    snapshot.metadata.game_version = "allocator_recovery_test";

    auto grid = workpieces::WorkpieceGrid::create({1, 1, 1});
    assert(grid);
    snapshot.workpieces.push_back({core::WorkpieceId::from_value(700),
                                   core::PrototypeId::parse("test:workpieces/recovery").value(),
                                   {1, 1, 1},
                                   workpieces::WorkpieceGridTextCodec::encode(grid.value())});
    build::BuildPieceRecord fire_owner;
    fire_owner.object_id = core::SaveId::from_value(800);
    fire_owner.prototype_id =
        core::PrototypeId::parse("test:build_pieces/fire_owner").value();
    snapshot.build_pieces.push_back(fire_owner);
    snapshot.fires.push_back({core::SaveId::from_value(800),
                              core::PrototypeId::parse("test:fire/recovery").value(),
                              simulation::FireState::unlit});
    snapshot.missing_prototypes.push_back(
        {world::MissingPrototypeKind::workpiece,
         900,
         core::PrototypeId::parse("missing:workpieces/recovery").value(),
         {},
         {},
         "saved",
         "test"});
    snapshot.missing_prototypes.push_back(
        {world::MissingPrototypeKind::process,
         1'000,
         core::PrototypeId::parse("missing:processes/recovery").value(),
         {},
         {},
         "saved",
         "test"});

    auto imported = world::WorldSnapshotBridge::import_snapshot(snapshot);
    assert(imported);
    assert(imported.value().save_ids().peek_next() == core::SaveId::from_value(901));
    assert(imported.value().process_ids().peek_next() == core::ProcessId::from_value(1'001));

    auto orphaned_fire = snapshot;
    orphaned_fire.fires.front().fire_id = core::SaveId::from_value(799);
    auto rejected_orphaned_fire = world::WorldSnapshotBridge::import_snapshot(orphaned_fire);
    assert(!rejected_orphaned_fire);
    assert(rejected_orphaned_fire.error().code == "world_snapshot.missing_owner");

    auto exhausted_save_ids = snapshot;
    exhausted_save_ids.missing_prototypes[0].stable_id = std::numeric_limits<std::uint64_t>::max();
    auto rejected_save_ids = world::WorldSnapshotBridge::import_snapshot(exhausted_save_ids);
    assert(!rejected_save_ids);
    assert(rejected_save_ids.error().code == "world_snapshot.id_range_exhausted");

    auto exhausted_process_ids = snapshot;
    exhausted_process_ids.missing_prototypes[1].stable_id =
        std::numeric_limits<std::uint64_t>::max();
    auto rejected_process_ids = world::WorldSnapshotBridge::import_snapshot(exhausted_process_ids);
    assert(!rejected_process_ids);
    assert(rejected_process_ids.error().code == "world_snapshot.id_range_exhausted");
}

void test_palette_fingerprint_changes_are_blocking() {
    heartstead::save::SaveMetadata metadata;
    metadata.game_version = "test";
    metadata.enabled_mods.push_back({"base", "1", "saved-hash"});

    std::vector<heartstead::modding::ModPrototypeFingerprint> active{
        {"base", "1", "changed-hash", 1}};
    auto changed = heartstead::save::SaveCompatibilityChecker::compare(metadata, active);
    assert(changed.has_errors());
    assert(std::ranges::any_of(changed.issues, [](const auto& issue) {
        return issue.code == "save_compatibility.prototype_hash_mismatch" &&
               issue.severity == heartstead::save::SaveCompatibilitySeverity::error;
    }));

    active = {{"base", "1", "saved-hash", 1}, {"extra", "1", "extra-hash", 1}};
    auto extra = heartstead::save::SaveCompatibilityChecker::compare(metadata, active);
    assert(extra.has_errors());
    assert(std::ranges::any_of(extra.issues, [](const auto& issue) {
        return issue.code == "save_compatibility.extra_active_mod" &&
               issue.severity == heartstead::save::SaveCompatibilitySeverity::error;
    }));
}

} // namespace

int main() {
    test_failed_stream_load_does_not_publish_chunk();
    test_worldgen_preserves_i64_height_and_rejects_unrepresentable_chunks();
    test_worldgen_features_only_belong_to_their_surface_chunk();
    test_chunk_interest_planning_is_bounded();
    test_saved_edit_batch_validation_is_atomic();
    test_saved_edits_ignore_derived_light_when_matching_the_baseline();
    test_chunk_mutations_preflight_and_compact_edit_history();
    test_chunk_residency_changes_invalidate_shared_seams();
    test_stream_reload_preserves_one_canonical_delta();
    test_world_set_voxel_rejects_unloaded_chunk();
    test_process_advance_all_is_atomic();
    test_snapshot_allocator_recovery_is_complete_and_overflow_safe();
    test_palette_fingerprint_changes_are_blocking();
    return 0;
}
