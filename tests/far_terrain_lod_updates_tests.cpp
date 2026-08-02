#include "engine/renderer/terrain/far_terrain_lod_updates.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <set>
#include <vector>

using namespace heartstead;

namespace {

renderer::FarTerrainPatch patch(std::uint32_t level, std::int64_t x, double minimum_x,
                                float priority) {
    renderer::FarTerrainPatch result;
    result.key = {level, x, 0, renderer::FarTerrainDomain::surface};
    result.horizontal_bounds = {{minimum_x, 0.0, 0.0}, {minimum_x + 32.0, 0.0, 32.0}};
    result.cell_size = static_cast<double>(1U << level);
    result.resolution = 8;
    result.streaming_priority = priority;
    return result;
}

renderer::FarTerrainPlan plan(std::vector<renderer::FarTerrainPatch> patches) {
    renderer::FarTerrainPlan result;
    result.patches = std::move(patches);
    return result;
}

void publish_all(renderer::FarTerrainLodUpdateGraph& graph,
                 const std::vector<renderer::FarTerrainLodUpdateRequest>& requests) {
    for (const auto& request : requests) {
        assert(graph.accepts_result(request.patch.key, request.request_revision));
        assert(graph.publish(request.patch.key, request.request_revision));
    }
}

} // namespace

int main() {
    renderer::FarTerrainLodUpdateConfig config;
    config.mid_level_count = 2;
    config.maximum_mid_rebuilds_per_frame = 2;
    config.maximum_far_rebuilds_per_frame = 1;

    assert(!renderer::FarTerrainLodUpdateGraph::create(config, 4, 2));
    auto created = renderer::FarTerrainLodUpdateGraph::create(config, 4, 4);
    assert(created);
    auto graph = std::move(created.value());

    const auto initial_plan = plan({patch(0, 0, 0.0, 0.9F), patch(1, 1, 0.0, 0.8F),
                                    patch(2, 2, 0.0, 0.7F), patch(3, 3, 0.0, 0.6F)});
    assert(graph.synchronize(initial_plan, 11));
    assert(graph.stats().desired_patches == 4);
    assert(graph.stats().missing_patches == 4);
    auto initial = graph.schedule_updates();
    assert(initial.size() == 4);
    assert(initial[0].band == renderer::FarTerrainLodBand::mid);
    assert(initial[1].band == renderer::FarTerrainLodBand::mid);
    assert(initial[2].band == renderer::FarTerrainLodBand::far);
    assert(initial[3].band == renderer::FarTerrainLodBand::far);
    for (const auto& request : initial) {
        assert(!request.replaces_resident_patch);
        assert(request.source_revision == 11);
    }
    publish_all(graph, initial);
    assert(graph.stats().current_patches == 4);
    assert(graph.stats().missing_patches == 0);

    // One authoritative edit fans out to both mid levels and both far levels. The graph retains
    // every old resident patch, reserves both mid rebuilds plus one far rebuild, and leaves the
    // remaining far patch pending instead of creating a hole.
    const std::array edit_regions{
        math::Bounds3d{{8.0, 0.0, 8.0}, {9.0, 1.0, 9.0}},
    };
    assert(graph.synchronize(initial_plan, 12, edit_regions));
    assert(graph.stats().stale_resident_patches == 4);
    assert(graph.stats().pending_mid_updates == 2);
    assert(graph.stats().pending_far_updates == 2);
    auto first_edit_wave = graph.schedule_updates();
    assert(first_edit_wave.size() == 3);
    assert(first_edit_wave[0].band == renderer::FarTerrainLodBand::mid);
    assert(first_edit_wave[1].band == renderer::FarTerrainLodBand::mid);
    assert(first_edit_wave[2].band == renderer::FarTerrainLodBand::far);
    for (const auto& request : first_edit_wave) {
        assert(request.replaces_resident_patch);
        assert(request.source_revision == 12);
    }
    publish_all(graph, first_edit_wave);
    assert(graph.stats().current_patches == 3);
    assert(graph.stats().stale_resident_patches == 1);
    auto second_edit_wave = graph.schedule_updates();
    assert(second_edit_wave.size() == 1);
    assert(second_edit_wave.front().band == renderer::FarTerrainLodBand::far);
    publish_all(graph, second_edit_wave);
    assert(graph.stats().current_patches == 4);

    // A transition-band move changes baked mesh input even when authoritative surface content is
    // unchanged. Its monotonic request ticket advances independently from the source hash, while a
    // priority-only plan update leaves resident geometry current.
    auto shifted_plan = initial_plan;
    shifted_plan.patches[0].finer_coverage = math::Bounds3d{{-16.0, 0.0, -16.0}, {16.0, 0.0, 16.0}};
    const auto before_geometry_request = graph.requested_revision(shifted_plan.patches[0].key);
    assert(before_geometry_request.has_value());
    assert(graph.synchronize(shifted_plan, 12));
    assert(!graph.is_current(shifted_plan.patches[0].key));
    auto geometry_update = graph.schedule_updates();
    assert(geometry_update.size() == 1);
    assert(geometry_update.front().source_revision == 12);
    assert(geometry_update.front().request_revision > *before_geometry_request);
    publish_all(graph, geometry_update);
    shifted_plan.patches[0].streaming_priority = 0.01F;
    assert(graph.synchronize(shifted_plan, 12));
    assert(graph.is_current(shifted_plan.patches[0].key));
    assert(graph.schedule_updates().empty());

    // Restore the original transition geometry before exercising content supersession.
    assert(graph.synchronize(initial_plan, 12));
    publish_all(graph, graph.schedule_updates());

    // A newer edit can supersede work already handed to a worker. Only the exact current ticket is
    // publishable; rejecting the stale result reopens the patch at the newest source revision.
    assert(graph.synchronize(initial_plan, 13, edit_regions));
    auto superseded = graph.schedule_updates();
    assert(superseded.size() == 3);
    const auto stale_ticket = superseded.front();
    assert(graph.synchronize(initial_plan, 14, edit_regions));
    assert(!graph.accepts_result(stale_ticket.patch.key, stale_ticket.request_revision));
    assert(graph.reject_stale(stale_ticket.patch.key, stale_ticket.request_revision));
    assert(graph.stats().total_coalesced_invalidations >= 3);
    for (std::size_t index = 1; index < superseded.size(); ++index) {
        assert(graph.reject_stale(superseded[index].patch.key, superseded[index].request_revision));
    }
    std::set<renderer::FarTerrainPatchKey> latest_keys;
    for (std::size_t frame = 0; frame < 2; ++frame) {
        const auto latest = graph.schedule_updates();
        for (const auto& request : latest) {
            assert(request.source_revision == 14);
            assert(latest_keys.insert(request.patch.key).second);
        }
        publish_all(graph, latest);
    }
    assert(latest_keys.size() == 4);
    assert(graph.stats().current_patches == 4);
    assert(graph.stats().total_stale_results == 3);

    // Region-specific invalidation leaves non-intersecting patches current, while an empty region
    // list on a changed revision deliberately means the caller could not localize the change.
    const auto split_plan = plan({patch(0, 0, 0.0, 0.9F), patch(0, 1, 64.0, 0.8F)});
    auto split_created = renderer::FarTerrainLodUpdateGraph::create(config, 4, 4);
    assert(split_created);
    auto split = std::move(split_created.value());
    assert(split.synchronize(split_plan, 20));
    publish_all(split, split.schedule_updates());
    assert(split.synchronize(split_plan, 21, edit_regions));
    assert(!split.is_current(split_plan.patches[0].key));
    assert(split.is_current(split_plan.patches[1].key));
    publish_all(split, split.schedule_updates());
    assert(split.synchronize(split_plan, 22));
    assert(split.stats().stale_resident_patches == 2);

    // Desired-set removal drops dependency state and makes later scheduling impossible.
    const auto removed_key = split_plan.patches[1].key;
    assert(split.synchronize(plan({split_plan.patches[0]}), 22));
    assert(!split.contains(removed_key));
    assert(split.stats().desired_patches == 1);

    return 0;
}
