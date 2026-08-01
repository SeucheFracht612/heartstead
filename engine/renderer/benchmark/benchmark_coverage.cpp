#include "engine/renderer/benchmark/benchmark_coverage.hpp"

#include <array>
#include <ranges>

namespace heartstead::renderer::benchmark {

namespace {

constexpr std::array coverage{
    BenchmarkCoverageEntry{"terrain", BenchmarkSceneKind::mountainous_terrain,
                           "editable terrain, greedy meshes, far terrain, shadows"},
    BenchmarkCoverageEntry{"forest", BenchmarkSceneKind::forest_cross_planes,
                           "instancing, cutouts, wind, occlusion, UI"},
    BenchmarkCoverageEntry{"crop-fields", BenchmarkSceneKind::starting_biome,
                           "dense vegetation, growth variation, wind"},
    BenchmarkCoverageEntry{"characters", BenchmarkSceneKind::character_workshop,
                           "skinning, morphs, animation LOD, shadows"},
    BenchmarkCoverageEntry{"equipment", BenchmarkSceneKind::character_workshop,
                           "socket attachments, visibility groups, materials"},
    BenchmarkCoverageEntry{"dense-settlement", BenchmarkSceneKind::light_heavy_settlement,
                           "retained props, local lights, cascaded shadows"},
    BenchmarkCoverageEntry{"active-workshop", BenchmarkSceneKind::character_workshop,
                           "visual-prefab states, animation, attachments, effects"},
    BenchmarkCoverageEntry{"many-local-lights", BenchmarkSceneKind::light_heavy_settlement,
                           "cluster construction and local-shadow selection"},
    BenchmarkCoverageEntry{"cave", BenchmarkSceneKind::dense_caves,
                           "underground visibility and chunk meshing"},
    BenchmarkCoverageEntry{"rapid-edits", BenchmarkSceneKind::rapid_voxel_edits,
                           "invalidation, background meshing, upload budgets"},
    BenchmarkCoverageEntry{"large-coordinates", BenchmarkSceneKind::large_coordinates,
                           "floating origin and deterministic material mapping"},
    BenchmarkCoverageEntry{"fast-traversal", BenchmarkSceneKind::high_speed_flythrough,
                           "visibility, streaming priority, far terrain"},
    BenchmarkCoverageEntry{"streaming", BenchmarkSceneKind::chunk_load_unload_churn,
                           "load cancellation, residency, deferred destruction"},
    BenchmarkCoverageEntry{"water", BenchmarkSceneKind::active_water,
                           "fluid updates, flow presentation, transparency"},
    BenchmarkCoverageEntry{"rain", BenchmarkSceneKind::starting_biome,
                           "weather particles, wetness, environment blending"},
    BenchmarkCoverageEntry{"fog", BenchmarkSceneKind::starting_biome,
                           "height fog, aerial perspective, transparent integration"},
    BenchmarkCoverageEntry{"night", BenchmarkSceneKind::light_heavy_settlement,
                           "exposure, emissive response, local-light budgets"},
    BenchmarkCoverageEntry{"particles", BenchmarkSceneKind::particle_stress,
                           "spawn budgets, pooling, material grouping"},
    BenchmarkCoverageEntry{"transparency", BenchmarkSceneKind::active_water,
                           "ordered transparent passes and depth softening"},
    BenchmarkCoverageEntry{"animation", BenchmarkSceneKind::character_workshop,
                           "blending, pose cache, motion vectors, animation LOD"},
    BenchmarkCoverageEntry{"resize", BenchmarkSceneKind::resize_minimize_stress,
                           "swapchain recreation and frame-local resources"},
};

} // namespace

std::span<const BenchmarkCoverageEntry> renderer_benchmark_coverage() noexcept {
    return coverage;
}

const BenchmarkCoverageEntry*
find_renderer_benchmark_coverage(std::string_view requirement) noexcept {
    const auto found = std::ranges::find(coverage, requirement,
                                         &BenchmarkCoverageEntry::requirement);
    return found == coverage.end() ? nullptr : &*found;
}

} // namespace heartstead::renderer::benchmark
