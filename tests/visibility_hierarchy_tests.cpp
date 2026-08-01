#include "engine/renderer/visibility/visibility_hierarchy.hpp"

#include <cassert>
#include <cmath>
#include <vector>

using namespace heartstead;

namespace {

renderer::VisibilityObject object(std::uint64_t key, double x, double size = 1.0) {
    return {key,
            {{x, 0.0, 0.0}, {x + size, size, size}},
            {{0, 0.0F, true}, {1, 0.05F, true}, {2, 0.2F, true}},
            0.0,
            1.0F,
            true,
            true};
}

renderer::VisibilityView view(std::uint32_t id, double x = 0.0) {
    renderer::VisibilityView result;
    result.id = id;
    result.camera_world = {x, 0.0, 0.0};
    result.viewport_height = 1080;
    result.vertical_field_of_view_radians = 1.0471976F;
    return result;
}

} // namespace

int main() {
    renderer::VisibilityHierarchy hierarchy;
    std::vector<renderer::VisibilityObject> objects;
    for (std::uint64_t index = 0; index < 24; ++index) {
        objects.push_back(object(100U + index, static_cast<double>(index) * 10.0 + 10.0));
    }
    objects[0].importance = 4.0F;
    objects[1].maximum_distance = 5.0;
    objects[2].casts_shadow = false;
    hierarchy.rebuild(objects);
    assert(hierarchy.object_count() == objects.size());
    assert(hierarchy.node_count() > 1);

    auto main_view = view(1);
    main_view.maximum_distance = 90.0;
    const auto first = hierarchy.query(std::span{&main_view, 1U});
    assert(first.stats.distance_culled > 0);
    assert(first.stats.selections_emitted > 0);
    assert(first.selections.front().key == 100U);
    // Nearby geometry needs LOD 0, while distant geometry can meet the same pixel-error budget
    // using a coarser resident mesh.
    assert(first.selections.front().lod_level == 0U);
    bool found_coarse = false;
    for (const auto& selected : first.selections) {
        found_coarse = found_coarse || selected.lod_level > 0U;
    }
    assert(found_coarse);

    auto shadow_view = view(2);
    shadow_view.kind = renderer::VisibilityViewKind::directional_shadow;
    shadow_view.maximum_distance = 40.0;
    const auto views = std::vector{main_view, shadow_view};
    const auto multi_view = hierarchy.query(views);
    assert(multi_view.stats.shadow_policy_culled == 1U);
    for (std::size_t index = 1; index < multi_view.selections.size(); ++index) {
        assert(multi_view.selections[index - 1].view_id <= multi_view.selections[index].view_id);
    }

    renderer::TemporalOcclusionHistory history;
    history.begin_frame(10);
    history.submit(1, 100, true);
    assert(!history.is_conservatively_occluded(1, 100));
    history.begin_frame(11);
    history.submit(1, 100, true);
    assert(history.is_conservatively_occluded(1, 100));

    renderer::VisibilityHierarchy::QueryOptions occlusion_options;
    occlusion_options.minimum_occlusion_distance = 0.0F;
    occlusion_options.occlusion_history = &history;
    const auto occluded = hierarchy.query(std::span{&main_view, 1U}, occlusion_options);
    assert(occluded.stats.occlusion_culled == 1U);
    assert(occluded.selections.front().key != 100U);

    history.begin_frame(12, true);
    assert(!history.is_conservatively_occluded(1, 100));

    // Large coordinates stay precise because the hierarchy subtracts the camera in double before
    // converting bounds to the camera-relative float frustum space.
    const double large = 8'000'000'000.0;
    const auto large_object = object(900, large + 25.0);
    hierarchy.rebuild(std::span{&large_object, 1U});
    const auto large_view = view(7, large);
    const auto large_result = hierarchy.query(std::span{&large_view, 1U});
    assert(large_result.selections.size() == 1U);
    assert(std::abs(large_result.selections.front().distance - 25.0) < 0.001);

    auto moved_object = large_object;
    moved_object.world_bounds = {{large + 250.0, 0.0, 0.0},
                                 {large + 251.0, 1.0, 1.0}};
    hierarchy.upsert(moved_object);
    const auto moved_result = hierarchy.query(std::span{&large_view, 1U});
    assert(moved_result.selections.size() == 1U);
    assert(std::abs(moved_result.selections.front().distance - 250.0) < 0.001);
    assert(hierarchy.erase(900));
    assert(!hierarchy.erase(900));
    assert(hierarchy.object_count() == 0U);
    hierarchy.clear();
    assert(hierarchy.node_count() == 0U);

    // Presentation systems create and destroy thousands of render primitives in one transaction.
    // Topology edits remain addressable before the deferred deterministic rebuild, including the
    // swap-and-pop index update used by removals.
    constexpr std::uint64_t batch_size = 4'096U;
    for (std::uint64_t index = 0; index < batch_size; ++index) {
        hierarchy.upsert(object(10'000U + index, static_cast<double>(index)));
    }
    assert(hierarchy.object_count() == batch_size);
    assert(hierarchy.node_count() > 1U);
    for (std::uint64_t index = 0; index < batch_size; index += 2U) {
        assert(hierarchy.erase(10'000U + index));
    }
    assert(hierarchy.object_count() == batch_size / 2U);
    for (std::uint64_t index = 1; index < batch_size; index += 2U) {
        assert(hierarchy.erase(10'000U + index));
    }
    assert(hierarchy.object_count() == 0U);
    assert(hierarchy.node_count() == 0U);

    return 0;
}
