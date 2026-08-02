#include "engine/renderer/terrain/far_terrain_mesh_scheduler.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace heartstead;

[[nodiscard]] renderer::FarTerrainLodUpdateRequest
update_request(const renderer::FarTerrainPatch& patch, std::uint64_t revision,
               std::uint64_t sequence, renderer::FarTerrainLodBand band) {
    renderer::FarTerrainLodUpdateRequest result;
    result.patch = patch;
    result.band = band;
    result.source_revision = revision;
    result.request_revision = revision;
    result.request_sequence = sequence;
    return result;
}

[[nodiscard]] renderer::FarTerrainMeshRequest
mesh_request(renderer::FarTerrainMeshScheduler& scheduler,
             const renderer::FarTerrainClipmap& clipmap, const renderer::FarTerrainPatch& patch,
             std::uint64_t revision, std::uint64_t sequence, renderer::FarTerrainLodBand band) {
    const auto row = static_cast<std::size_t>(patch.resolution) + 3U;
    auto reusable = scheduler.acquire_surface_samples(row * row);
    auto surface = clipmap.capture_patch_surface(
        patch,
        [revision](double x, double z, renderer::FarTerrainDomain) {
            return renderer::FarTerrainSurfaceSample{
                static_cast<double>(revision) + x * 0.001 + z * 0.002, 5};
        },
        std::move(reusable));
    assert(surface);
    return {update_request(patch, revision, sequence, band), std::move(surface).value()};
}

[[nodiscard]] std::vector<renderer::FarTerrainMeshResult>
wait_for_results(renderer::FarTerrainMeshScheduler& scheduler, std::size_t expected) {
    std::vector<renderer::FarTerrainMeshResult> results;
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

} // namespace

int main() {
    using namespace heartstead;

    renderer::FarTerrainMeshSchedulerConfig invalid;
    invalid.worker_count = 0;
    assert(!invalid.validate());

    auto created_clipmap = renderer::FarTerrainClipmap::create({3, 5, 128, 2.0, 2'048.0});
    assert(created_clipmap);
    auto clipmap = std::move(created_clipmap).value();
    const auto plan = clipmap.plan({0.0, 40.0, 0.0});
    assert(plan.patches.size() >= 3U);

    renderer::FarTerrainMeshSchedulerConfig config;
    config.worker_count = 1;
    config.maximum_concurrent_jobs = 2;
    config.maximum_completed_results = 4;
    config.maximum_cached_surface_buffers = 4;
    config.maximum_cached_mesh_buffers = 2;
    auto created_scheduler = renderer::FarTerrainMeshScheduler::create(clipmap, config);
    assert(created_scheduler);
    auto scheduler = std::move(created_scheduler).value();

    auto expensive =
        mesh_request(*scheduler, clipmap, plan.patches[0], 10, 1, renderer::FarTerrainLodBand::mid);
    auto cancelled =
        mesh_request(*scheduler, clipmap, plan.patches[1], 11, 2, renderer::FarTerrainLodBand::far);
    const auto cancelled_key = cancelled.update.patch.key;
    assert(scheduler->submit(std::move(expensive)));
    assert(scheduler->submit(std::move(cancelled)));
    assert(!scheduler->has_capacity());
    assert(scheduler->in_flight_tickets().size() == 2U);
    scheduler->cancel(cancelled_key);

    auto duplicate =
        mesh_request(*scheduler, clipmap, plan.patches[1], 12, 3, renderer::FarTerrainLodBand::far);
    const auto coalesced = scheduler->submit(std::move(duplicate));
    assert(!coalesced);
    assert(coalesced.error().code == "renderer.far_terrain_mesh_request_coalesced");

    auto results = wait_for_results(*scheduler, 2);
    const auto cancelled_result = std::ranges::find_if(results, [&cancelled_key](const auto& item) {
        return item.update.patch.key == cancelled_key;
    });
    assert(cancelled_result != results.end());
    assert(cancelled_result->state == renderer::FarTerrainMeshResultState::cancelled);
    assert(!cancelled_result->mesh.has_value());
    assert(scheduler->stats().cancelled_jobs == 1);
    assert(scheduler->stats().pooled_surface_buffers > 0);
    assert(scheduler->stats().pooled_surface_sample_capacity > 0);
    assert(!scheduler->has_in_flight(cancelled_key));

    const auto succeeded = std::ranges::find_if(results, [](const auto& item) {
        return item.state == renderer::FarTerrainMeshResultState::succeeded;
    });
    assert(succeeded != results.end());
    assert(succeeded->mesh.has_value());
    assert(succeeded->update.request_revision == 10U);
    const auto* recycled_vertices = succeeded->mesh->vertices.data();
    const auto* recycled_indices = succeeded->mesh->indices.data();
    scheduler->recycle_mesh(std::move(*succeeded->mesh));
    assert(scheduler->stats().pooled_mesh_buffers == 1U);

    auto reuse =
        mesh_request(*scheduler, clipmap, plan.patches[2], 13, 4, renderer::FarTerrainLodBand::mid);
    assert(scheduler->submit(std::move(reuse)));
    auto reused = wait_for_results(*scheduler, 1);
    assert(reused.front().state == renderer::FarTerrainMeshResultState::succeeded);
    assert(reused.front().mesh.has_value());
    assert(reused.front().mesh->vertices.data() == recycled_vertices);
    assert(reused.front().mesh->indices.data() == recycled_indices);
    scheduler->recycle_mesh(std::move(*reused.front().mesh));

    auto malformed =
        mesh_request(*scheduler, clipmap, plan.patches[0], 14, 5, renderer::FarTerrainLodBand::far);
    malformed.surface.samples.pop_back();
    const auto rejected = scheduler->submit(std::move(malformed));
    assert(!rejected);
    assert(rejected.error().code == "renderer.invalid_far_terrain_mesh_request");

    scheduler->shutdown();
    assert(!scheduler->submit(mesh_request(*scheduler, clipmap, plan.patches[0], 15, 6,
                                           renderer::FarTerrainLodBand::far)));
    return 0;
}
