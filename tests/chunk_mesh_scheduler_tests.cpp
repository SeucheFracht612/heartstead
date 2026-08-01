#include "engine/renderer/chunks/chunk_mesh_scheduler.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

using heartstead::renderer::ChunkMeshRequest;
using heartstead::renderer::ChunkMeshResult;
using heartstead::renderer::ChunkMeshScheduler;
using heartstead::world::BlockRenderTableSnapshot;
using heartstead::world::ChunkIdentity;

[[nodiscard]] std::shared_ptr<const BlockRenderTableSnapshot> legacy_render_table() {
    auto table = heartstead::world::build_block_render_table_snapshot(nullptr);
    assert(table);
    return std::make_shared<const BlockRenderTableSnapshot>(std::move(table).value());
}

[[nodiscard]] ChunkMeshRequest
make_request(ChunkMeshScheduler& scheduler, const heartstead::world::ChunkDatabase& chunks,
             ChunkIdentity identity, std::shared_ptr<const BlockRenderTableSnapshot> render_table,
             std::uint64_t sequence) {
    constexpr std::size_t snapshot_side =
        static_cast<std::size_t>(heartstead::world::VoxelChunk::edge_length) + 2;
    auto storage = scheduler.acquire_snapshot_cells(snapshot_side * snapshot_side * snapshot_side);
    auto snapshot = heartstead::world::build_chunk_neighborhood_snapshot(
        chunks, identity, *render_table, std::move(storage));
    assert(snapshot);

    ChunkMeshRequest request;
    request.identity = identity;
    request.stage_ticket =
        chunks.find(identity.coordinate)->stage_ticket(heartstead::world::ChunkStage::mesh);
    request.center_revision = snapshot.value().center_revision;
    request.block_render_table_revision = render_table->revision;
    request.neighborhood = std::move(snapshot).value();
    request.render_table = std::move(render_table);
    request.priority.sequence = sequence;
    return request;
}

[[nodiscard]] std::vector<ChunkMeshResult> wait_for_results(ChunkMeshScheduler& scheduler,
                                                            std::size_t expected) {
    std::vector<ChunkMeshResult> results;
    for (std::size_t attempt = 0; attempt < 5'000 && results.size() < expected; ++attempt) {
        auto completed = scheduler.drain_completed();
        for (auto& result : completed) {
            results.push_back(std::move(result));
        }
        if (results.size() < expected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    assert(results.size() == expected);
    return results;
}

void fill_checkerboard(heartstead::world::VoxelChunk& chunk) {
    for (std::uint16_t z = 0; z < heartstead::world::VoxelChunk::edge_length; ++z) {
        for (std::uint16_t y = 0; y < heartstead::world::VoxelChunk::edge_length; ++y) {
            for (std::uint16_t x = 0; x < heartstead::world::VoxelChunk::edge_length; ++x) {
                if ((x + y + z) % 2U == 0U) {
                    assert(chunk.set({x, y, z}, heartstead::world::VoxelCell{1, 255}));
                }
            }
        }
    }
}

void test_scheduler_coalesces_and_cancels_queued_work() {
    using namespace heartstead;
    renderer::ChunkMeshSchedulerConfig config;
    config.worker_count = 1;
    config.max_concurrent_jobs = 2;
    config.max_completed_results = 8;
    config.max_cached_snapshot_buffers = 4;
    auto scheduler_result = renderer::ChunkMeshScheduler::create(config);
    assert(scheduler_result);
    auto scheduler = std::move(scheduler_result).value();

    world::WorldState world;
    auto& expensive = world.chunks().get_or_create({0, 0, 0});
    fill_checkerboard(expensive);
    auto& cancelled = world.chunks().get_or_create({4, 0, 0});
    assert(cancelled.set({1, 1, 1}, world::VoxelCell{1, 255}));
    const auto table = legacy_render_table();

    auto first = make_request(*scheduler, world.chunks(), expensive.identity(), table, 1);
    first.priority.visible = true;
    first.priority.missing_resident_mesh = true;
    auto second = make_request(*scheduler, world.chunks(), cancelled.identity(), table, 2);
    assert(scheduler->submit(std::move(first)));
    assert(scheduler->submit(std::move(second)));
    scheduler->cancel(cancelled.identity());

    auto duplicate = make_request(*scheduler, world.chunks(), cancelled.identity(), table, 3);
    const auto coalesced = scheduler->submit(std::move(duplicate));
    assert(!coalesced);
    assert(coalesced.error().code == "renderer.chunk_mesh_request_coalesced");

    auto results = wait_for_results(*scheduler, 2);
    const auto cancelled_result =
        std::ranges::find(results, cancelled.identity(), &ChunkMeshResult::identity);
    assert(cancelled_result != results.end());
    assert(cancelled_result->state == renderer::ChunkMeshResultState::cancelled);
    assert(scheduler->stats().cancelled_jobs == 1);
    assert(scheduler->stats().pooled_snapshot_buffers > 0);
    assert(!scheduler->has_in_flight(cancelled.identity()));

    const auto completed_mesh =
        std::ranges::find(results, expensive.identity(), &ChunkMeshResult::identity);
    assert(completed_mesh != results.end());
    assert(completed_mesh->mesh.has_value());
    const auto* recycled_vertices = completed_mesh->mesh->vertices.data();
    const auto* recycled_indices = completed_mesh->mesh->indices.data();
    scheduler->recycle_mesh(std::move(*completed_mesh->mesh));
    assert(scheduler->stats().pooled_mesh_buffers == 1);
    assert(scheduler->stats().pooled_mesh_vertex_capacity > 0);
    assert(scheduler->stats().pooled_mesh_index_capacity > 0);

    auto reuse = make_request(*scheduler, world.chunks(), expensive.identity(), table, 4);
    assert(scheduler->submit(std::move(reuse)));
    auto reused_results = wait_for_results(*scheduler, 1);
    assert(reused_results.front().mesh.has_value());
    assert(reused_results.front().mesh->vertices.data() == recycled_vertices);
    assert(reused_results.front().mesh->indices.data() == recycled_indices);
    scheduler->recycle_mesh(std::move(*reused_results.front().mesh));
}

void test_results_carry_stale_center_neighbor_table_and_generation_metadata() {
    using namespace heartstead;
    renderer::ChunkMeshSchedulerConfig config;
    config.worker_count = 1;
    config.max_concurrent_jobs = 1;
    config.max_completed_results = 8;
    config.max_cached_snapshot_buffers = 2;
    config.meshing_mode = renderer::ChunkMeshingMode::reference;
    auto scheduler_result = renderer::ChunkMeshScheduler::create(config);
    assert(scheduler_result);
    auto scheduler = std::move(scheduler_result).value();

    world::WorldState world;
    const world::ChunkCoord center_coord{0, 0, 0};
    const world::ChunkCoord neighbor_coord{1, 0, 0};
    assert(world.chunks().set(center_coord, {31, 5, 5}, world::VoxelCell{1, 255}));
    assert(world.chunks().set(neighbor_coord, {0, 5, 5}, world::VoxelCell{1, 255}));
    const auto original_identity = world.chunks().find(center_coord)->identity();
    const auto table = legacy_render_table();

    auto old_request = make_request(*scheduler, world.chunks(), original_identity, table, 1);
    const auto requested_revision = old_request.center_revision;
    assert(scheduler->submit(std::move(old_request)));

    assert(world.chunks().set(center_coord, {30, 5, 5}, world::VoxelCell{1, 255}));
    assert(world.chunks().set(neighbor_coord, {0, 5, 5}, world::VoxelCell{2, 255}));
    auto results = wait_for_results(*scheduler, 1);
    assert(results.front().state == renderer::ChunkMeshResultState::succeeded);
    assert(results.front().center_revision == requested_revision);
    assert(world.chunks().find(center_coord)->content_revision() != requested_revision);
    assert(
        !world::dependency_revisions_match(world.chunks(), results.front().dependency_revisions));

    BlockRenderTableSnapshot newer_table = *table;
    ++newer_table.revision;
    assert(results.front().block_render_table_revision != newer_table.revision);

    auto generation_request = make_request(*scheduler, world.chunks(), original_identity, table, 2);
    assert(scheduler->submit(std::move(generation_request)));
    assert(world.chunks().erase(center_coord));
    auto& replacement = world.chunks().get_or_create(center_coord);
    assert(replacement.set({2, 2, 2}, world::VoxelCell{1, 255}));
    assert(replacement.identity() != original_identity);

    results = wait_for_results(*scheduler, 1);
    assert(results.front().identity == original_identity);
    assert(world.chunks().find(center_coord)->identity() != results.front().identity);
    assert(
        !world::dependency_revisions_match(world.chunks(), results.front().dependency_revisions));
}

} // namespace

int main() {
    test_scheduler_coalesces_and_cancels_queued_work();
    test_results_carry_stale_center_neighbor_table_and_generation_metadata();
    return 0;
}
