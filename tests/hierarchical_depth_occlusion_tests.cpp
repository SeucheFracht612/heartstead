#include "engine/renderer/visibility/hierarchical_depth_occlusion.hpp"

#include <array>
#include <cassert>

int main() {
    using namespace heartstead;

    renderer::RenderCamera camera;
    camera.local_position = {0.0F, 0.0F, 0.0F};
    camera.near_plane = 0.1F;
    camera.far_plane = 1'000.0F;
    assert(camera.update_matrices());

    renderer::HierarchicalDepthOcclusion hierarchy;
    assert(hierarchy.initialize({128, 72, 0.001F, 2, 24.0F, 0.35F}));
    const std::array occluders{
        math::Bounds3f{{-4.0F, -4.0F, -6.0F}, {4.0F, 4.0F, -5.0F}},
    };
    assert(hierarchy.rebuild(camera, occluders));
    const math::Bounds3f hidden{{-1.0F, -1.0F, -12.0F}, {1.0F, 1.0F, -10.0F}};
    const math::Bounds3f visible{{8.0F, -1.0F, -12.0F}, {10.0F, 1.0F, -10.0F}};
    assert(!hierarchy.query(1, hidden));
    assert(!hierarchy.query(2, visible));
    assert(hierarchy.rebuild(camera, occluders));
    assert(hierarchy.query(1, hidden));
    assert(!hierarchy.query(2, visible));
    assert(hierarchy.stats().mip_levels > 1);
    assert(hierarchy.stats().rasterized_occluders == 1);

    camera.local_position = {64.0F, 0.0F, 0.0F};
    assert(camera.update_matrices());
    assert(hierarchy.rebuild(camera, {}));
    assert(!hierarchy.query(1, hidden));
    assert(hierarchy.stats().camera_cut_resets == 1);

    hierarchy.erase(1);
    hierarchy.reset_history();
    return 0;
}
