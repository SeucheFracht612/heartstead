#include "engine/renderer/terrain/far_terrain_clipmap.hpp"

#include <cassert>
#include <cmath>
#include <set>

using namespace heartstead;

int main() {
    auto clipmap_result = renderer::FarTerrainClipmap::create(
        {5, 7, 8, 4.0, 10'000.0, 3.0F, renderer::FarTerrainDomain::surface});
    assert(clipmap_result);
    auto clipmap = std::move(clipmap_result.value());

    const auto first = clipmap.plan({8'000'000'000.25, 50.0, -8'000'000'000.25});
    const auto stable = clipmap.plan({8'000'000'001.25, 50.0, -8'000'000'001.25});
    assert(!first.patches.empty());
    assert(first.patches.size() == stable.patches.size());
    for (std::size_t index = 0; index < first.patches.size(); ++index) {
        assert(first.patches[index].key == stable.patches[index].key);
    }

    std::set<renderer::FarTerrainPatchKey> keys;
    for (const auto& patch : first.patches) {
        assert(keys.insert(patch.key).second);
        assert(patch.horizontal_bounds.is_valid());
        assert(patch.resolution == 8U);
    }

    const auto& patch = first.patches.front();
    std::size_t sample_count = 0;
    const auto sampler = [&sample_count](double x, double z, renderer::FarTerrainDomain) {
        ++sample_count;
        return renderer::FarTerrainSurfaceSample{10.0 + x * 0.000001 + z * 0.000002, 7};
    };
    auto mesh = clipmap.build_patch_mesh(patch, sampler);
    assert(mesh);
    assert(sample_count == 121U);
    assert(mesh.value().vertices.size() == 81U);
    assert(mesh.value().indices.size() == 384U);
    assert(mesh.value().local_bounds.is_valid());
    for (const auto& vertex : mesh.value().vertices) {
        assert(vertex.local_position.is_finite());
        assert(std::abs(math::length(vertex.normal) - 1.0) < 0.001);
        assert(vertex.material == 7U);
    }

    // The immutable bordered grid is a complete worker input. Later sampler changes cannot alter
    // its geometry, and malformed snapshots fail rather than indexing partial storage.
    double captured_height = 20.0;
    const renderer::FarTerrainSurfaceSampler mutable_sampler =
        [&captured_height](double, double, renderer::FarTerrainDomain) {
            return renderer::FarTerrainSurfaceSample{captured_height, 9};
        };
    auto captured = clipmap.capture_patch_surface(patch, mutable_sampler);
    assert(captured);
    captured_height = 80.0;
    auto captured_mesh = clipmap.build_patch_mesh(patch, captured.value());
    assert(captured_mesh);
    assert(captured_mesh.value().world_origin.y == 20.0);
    auto live_mesh = clipmap.build_patch_mesh(patch, mutable_sampler);
    assert(live_mesh);
    assert(live_mesh.value().world_origin.y == 80.0);
    captured.value().samples.pop_back();
    assert(!clipmap.build_patch_mesh(patch, captured.value()));

    const auto partial_sampler = [boundary = (patch.horizontal_bounds.min.x +
                                               patch.horizontal_bounds.max.x) *
                                              0.5](
                                     double x, double, renderer::FarTerrainDomain) {
        return renderer::FarTerrainSurfaceSample{12.0, 3, x <= boundary};
    };
    auto partial_mesh = clipmap.build_patch_mesh(patch, partial_sampler);
    assert(partial_mesh);
    assert(!partial_mesh.value().indices.empty());
    assert(partial_mesh.value().indices.size() < mesh.value().indices.size());
    const auto missing_sampler = [](double, double, renderer::FarTerrainDomain) {
        return renderer::FarTerrainSurfaceSample{0.0, 0, false};
    };
    auto missing_mesh = clipmap.build_patch_mesh(patch, missing_sampler);
    assert(missing_mesh);
    assert(missing_mesh.value().indices.empty());

    // Crossing a patch boundary replaces only the entering/leaving columns; it never changes the
    // world-space key or samples of patches that remain resident.
    const auto patch_span = 4.0 * 8.0;
    const auto moved = clipmap.plan({first.camera_world.x + patch_span, 50.0,
                                     first.camera_world.z});
    std::size_t shared = 0;
    std::set<renderer::FarTerrainPatchKey> moved_keys;
    for (const auto& moved_patch : moved.patches) {
        moved_keys.insert(moved_patch.key);
    }
    for (const auto& key : keys) {
        shared += moved_keys.contains(key) ? 1U : 0U;
    }
    assert(shared > first.patches.size() / 2U);

    return 0;
}
