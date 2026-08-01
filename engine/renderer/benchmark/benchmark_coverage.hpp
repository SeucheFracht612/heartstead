#pragma once

#include "engine/renderer/benchmark/benchmark_scene.hpp"

#include <span>
#include <string_view>

namespace heartstead::renderer::benchmark {

struct BenchmarkCoverageEntry {
    std::string_view requirement;
    BenchmarkSceneKind scene = BenchmarkSceneKind::flat_terrain;
    std::string_view exercised_systems;
};

[[nodiscard]] std::span<const BenchmarkCoverageEntry> renderer_benchmark_coverage() noexcept;
[[nodiscard]] const BenchmarkCoverageEntry*
find_renderer_benchmark_coverage(std::string_view requirement) noexcept;

} // namespace heartstead::renderer::benchmark
