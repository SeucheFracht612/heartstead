#include "engine/world/chunks/chunk_edit_delta_codec.hpp"
#include "engine/world/streaming/chunk_load_scheduler.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace heartstead;

class TestDeltaSource final : public world::IChunkEditDeltaSource {
  public:
    std::map<world::ChunkCoord, save::ChunkEditSaveRecord> records;

    [[nodiscard]] core::Result<std::optional<save::ChunkEditSaveRecord>>
    read_chunk_delta(world::ChunkCoord coord) const override {
        const auto found = records.find(coord);
        return core::Result<std::optional<save::ChunkEditSaveRecord>>::success(
            found == records.end() ? std::nullopt
                                   : std::optional<save::ChunkEditSaveRecord>(found->second));
    }
};

class TestChunkGenerator final : public world::IChunkLoadGenerator {
  public:
    [[nodiscard]] core::Result<world::VoxelChunk> generate(world::ChunkCoord coord) const override {
        std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells,
                                            world::VoxelCell{42, 7});
        world::VoxelChunk chunk(coord);
        auto status = chunk.load_generated_cells(std::move(cells));
        if (!status) {
            return core::Result<world::VoxelChunk>::failure(status.error().code,
                                                            status.error().message);
        }
        return core::Result<world::VoxelChunk>::success(std::move(chunk));
    }
};

[[nodiscard]] world::ChunkLoadSchedulerContext
make_context(std::shared_ptr<const world::IChunkEditDeltaSource> source = {}) {
    const auto clay = core::PrototypeId::parse("base:voxels/clay");
    assert(clay);

    world::VoxelDefinition definition;
    definition.type = 1;
    definition.prototype_id = clay.value();
    definition.display_name = "Clay";
    definition.terrain_material = "clay";
    definition.mining_tool = "shovel";

    world::RegionDescriptor valley;
    valley.id = "temperate_valley";
    valley.age = "settlement_age";
    valley.biome_cluster = "temperate";
    valley.resource_rules.push_back({clay.value(), "surface_deposit", 1.0});

    world::ChunkLoadSchedulerContext context;
    context.generation.world_seed = 12345;
    context.generation.region_id = valley.id;
    context.generation.base_surface_y = 8;
    assert(context.regions.add_region(std::move(valley)));
    assert(context.palette.add(std::move(definition)));
    context.saved_deltas = std::move(source);
    return context;
}

void wait_for_mailbox(world::ChunkLoadScheduler& scheduler, std::size_t expected) {
    for (std::size_t attempt = 0; attempt < 10'000; ++attempt) {
        if (scheduler.stats().completed_mailbox_count >= expected) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "chunk load workers did not complete before the test timeout");
}

void drain_until_idle(world::ChunkLoadScheduler& scheduler, world::WorldState& state,
                      std::vector<world::ChunkStreamLoadReport>& published,
                      std::vector<world::ChunkCoord>& cancelled,
                      std::vector<world::ChunkLoadFailure>& failures) {
    for (std::size_t attempt = 0; attempt < 10'000 && scheduler.has_in_flight(); ++attempt) {
        auto report = scheduler.update(state);
        for (auto& load : report.published) {
            published.push_back(std::move(load));
        }
        for (const auto coord : report.cancelled) {
            cancelled.push_back(coord);
        }
        for (auto& failure : report.failures) {
            failures.push_back(std::move(failure));
        }
        if (scheduler.has_in_flight()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    assert(!scheduler.has_in_flight());
}

void test_bounded_background_load_and_publication() {
    auto source = std::make_shared<TestDeltaSource>();
    const world::VoxelEditRecord edit{
        {0, 0, 0}, {1, 2, 3}, world::VoxelCell{1, 0}, world::VoxelCell{9, 4}};
    const std::vector<const world::VoxelEditRecord*> edits{&edit};
    source->records.emplace(
        edit.chunk_coord,
        save::ChunkEditSaveRecord{edit.chunk_coord,
                                  world::ChunkEditDeltaTextCodec::encode(edit.chunk_coord, edits)});

    world::ChunkLoadSchedulerConfig config;
    config.worker_count = 2;
    config.max_concurrent_requests = 2;
    config.max_completed_results = 2;
    config.max_publications_per_update = 1;
    auto created = world::ChunkLoadScheduler::create(make_context(source), config);
    assert(created);
    auto scheduler = std::move(created).value();

    auto first = scheduler->submit({0, 0, 0}, jobs::JobPriority::high);
    auto second = scheduler->submit({1, 0, 0});
    assert(first && second);
    auto duplicate = scheduler->submit({0, 0, 0});
    assert(!duplicate && duplicate.error().code == "chunk_load_scheduler.duplicate_request");
    auto full = scheduler->submit({2, 0, 0});
    assert(!full && full.error().code == "chunk_load_scheduler.full");
    assert(scheduler->stats().reserved_working_bytes == 128U * 1024U * 1024U);

    wait_for_mailbox(*scheduler, 2);
    world::WorldState state;
    auto first_publication = scheduler->update(state);
    assert(first_publication.processed_count() == 1);
    assert(first_publication.item_budget_exhausted);
    assert(scheduler->stats().ready_for_publication_count == 1);
    auto second_publication = scheduler->update(state);
    assert(second_publication.processed_count() == 1);
    assert(!scheduler->has_in_flight());
    assert(state.chunks().chunk_count() == 2);
    auto cell = state.chunks().get({0, 0, 0}, {1, 2, 3});
    assert(cell && cell.value() == (world::VoxelCell{9, 4}));
    assert(state.chunks().edit_log().size() == 1);
    assert(scheduler->stats().published_requests == 2);
    assert(scheduler->stats().reserved_working_bytes == 0);
    assert(scheduler->stats().reserved_working_bytes_high_water == 128U * 1024U * 1024U);
    assert(scheduler->stats().maximum_pipeline_latency_ms > 0.0);

    assert(scheduler->submit({6, 0, 0}));
    assert(scheduler->submit({7, 0, 0}));
    const std::array retained{world::ChunkCoord{7, 0, 0}};
    assert(scheduler->cancel_all_except(retained) == 1);

    std::vector<world::ChunkStreamLoadReport> published;
    std::vector<world::ChunkCoord> cancelled;
    std::vector<world::ChunkLoadFailure> failures;
    drain_until_idle(*scheduler, state, published, cancelled, failures);
    assert(failures.empty());
    assert(published.size() == 1 && published.front().coord == retained.front());
    const std::vector<world::ChunkCoord> expected_cancelled{{6, 0, 0}};
    assert(cancelled == expected_cancelled);
    assert(!state.chunks().contains({6, 0, 0}));
    assert(state.chunks().contains({7, 0, 0}));
}

void test_cancellation_failure_stale_and_memory_backpressure() {
    world::ChunkLoadSchedulerConfig config;
    config.worker_count = 1;
    config.max_concurrent_requests = 2;
    config.max_completed_results = 2;
    config.reservation_bytes_per_request = 4U * 1024U * 1024U;
    config.max_reserved_working_bytes = 4U * 1024U * 1024U;
    auto created = world::ChunkLoadScheduler::create(make_context(), config);
    assert(created);
    auto scheduler = std::move(created).value();
    auto first = scheduler->submit({2, 0, 0});
    assert(first);
    auto memory_rejected = scheduler->submit({3, 0, 0});
    assert(!memory_rejected && memory_rejected.error().code == "chunk_load_scheduler.full");
    assert(scheduler->cancel({2, 0, 0}));

    world::WorldState state;
    std::vector<world::ChunkStreamLoadReport> published;
    std::vector<world::ChunkCoord> cancelled;
    std::vector<world::ChunkLoadFailure> failures;
    drain_until_idle(*scheduler, state, published, cancelled, failures);
    assert(published.empty() && failures.empty());
    const std::vector<world::ChunkCoord> expected_cancelled{{2, 0, 0}};
    assert(cancelled == expected_cancelled);
    assert(scheduler->stats().cancelled_requests == 1);

    auto stale = scheduler->submit({4, 0, 0});
    assert(stale);
    wait_for_mailbox(*scheduler, 1);
    static_cast<void>(state.chunks().get_or_create({4, 0, 0}));
    auto stale_report = scheduler->update(state);
    const std::vector<world::ChunkCoord> expected_stale{{4, 0, 0}};
    assert(stale_report.stale == expected_stale);
    assert(scheduler->stats().stale_requests == 1);

    auto malformed_source = std::make_shared<TestDeltaSource>();
    malformed_source->records.emplace(world::ChunkCoord{5, 0, 0},
                                      save::ChunkEditSaveRecord{{5, 0, 0}, "not a chunk delta"});
    auto malformed_created =
        world::ChunkLoadScheduler::create(make_context(malformed_source), config);
    assert(malformed_created);
    auto malformed = std::move(malformed_created).value();
    assert(malformed->submit({5, 0, 0}));
    drain_until_idle(*malformed, state, published, cancelled, failures);
    assert(failures.size() == 1);
    assert(failures.front().error.code == "world_snapshot.invalid_chunk_delta_magic");
    assert(!state.chunks().contains({5, 0, 0}));
    assert(malformed->stats().failed_requests == 1);
    assert(malformed->stats().last_worker_ms > 0.0);
}

void test_chunk_delta_decode_record_limit() {
    std::string encoded = "heartstead.chunk_edit_delta.v2\ncoord=8|0|0\n";
    const std::string edit = "edit=0|0|0|1|0|0|0|2|0|0|0\n";
    encoded.reserve(encoded.size() + (world::VoxelChunk::total_cells + 1) * edit.size() + 4);
    for (std::size_t index = 0; index <= world::VoxelChunk::total_cells; ++index) {
        encoded += edit;
    }
    encoded += "end\n";

    auto decoded = world::ChunkEditDeltaTextCodec::decode({8, 0, 0}, encoded);
    assert(!decoded);
    assert(decoded.error().code == "world_snapshot.too_many_chunk_delta_edits");
}

void test_custom_generator_uses_bounded_publication_path() {
    world::ChunkLoadSchedulerContext context;
    context.generator = std::make_shared<TestChunkGenerator>();
    assert(context.validate());

    world::ChunkLoadSchedulerConfig config;
    config.worker_count = 1;
    config.max_concurrent_requests = 1;
    config.max_completed_results = 1;
    auto created = world::ChunkLoadScheduler::create(std::move(context), config);
    assert(created);
    auto scheduler = std::move(created).value();
    assert(scheduler->submit({9, 0, 0}));

    world::WorldState state;
    std::vector<world::ChunkStreamLoadReport> published;
    std::vector<world::ChunkCoord> cancelled;
    std::vector<world::ChunkLoadFailure> failures;
    drain_until_idle(*scheduler, state, published, cancelled, failures);
    assert(cancelled.empty() && failures.empty());
    assert(published.size() == 1);
    auto cell = state.chunks().get({9, 0, 0}, {0, 0, 0});
    assert(cell && cell.value() == (world::VoxelCell{42, 7}));
}

void test_validation_and_names() {
    world::ChunkLoadSchedulerConfig config;
    config.worker_count = 0;
    assert(!config.validate());
    config.worker_count = 1;
    config.max_concurrent_requests = 2;
    config.max_completed_results = 1;
    assert(!config.validate());
    config.max_completed_results = 2;
    config.reservation_bytes_per_request = 2;
    config.max_reserved_working_bytes = 1;
    assert(!config.validate());

    auto context = make_context();
    context.generation.region_id = "missing";
    assert(!context.validate());
    assert(std::string_view(world::chunk_load_result_state_name(
               static_cast<world::ChunkLoadResultState>(255))) == "unknown");
}

} // namespace

int main() {
    test_bounded_background_load_and_publication();
    test_cancellation_failure_stale_and_memory_backpressure();
    test_chunk_delta_decode_record_limit();
    test_custom_generator_uses_bounded_publication_path();
    test_validation_and_names();
    return 0;
}
