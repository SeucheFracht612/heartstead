#include "engine/world/meshing/voxel_meshing_benchmark.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace benchmark = heartstead::world::benchmark;

[[nodiscard]] const benchmark::VoxelMeshingBenchmarkSample*
find_sample(const benchmark::VoxelMeshingBenchmarkReport& report,
            benchmark::VoxelMeshingOperation operation, std::uint32_t repetition) {
    const auto found = std::ranges::find_if(report.raw_samples, [&](const auto& sample) {
        return sample.operation == operation && sample.repetition == repetition;
    });
    return found == report.raw_samples.end() ? nullptr : &*found;
}

void test_small_benchmark_retains_raw_samples_and_geometry() {
    benchmark::VoxelMeshingBenchmarkConfig config;
    config.corpora = {benchmark::VoxelCorpusKind::uniform_solid};
    config.warmup_repetitions = 0;
    config.repetitions = 2;
    auto report = benchmark::run_voxel_meshing_benchmark(config);
    assert(report);
    assert(report.value().validate());
    assert(report.value().geometry.size() == 1);
    assert(report.value().raw_samples.size() == 8);
    assert(report.value().summaries().size() == 4);

    const auto& geometry = report.value().geometry.front();
    assert(geometry.corpus_stats.cell_count == 32U * 32U * 32U);
    assert(geometry.corpus_stats.non_air_count == geometry.corpus_stats.cell_count);
    assert(geometry.visible_unit_face_count == 6U * 32U * 32U);
    assert(geometry.reference.merged_face_count == geometry.visible_unit_face_count);
    assert(geometry.greedy.merged_face_count == 6);
    assert(geometry.reference.directional_unit_surface_faces ==
           geometry.greedy.directional_unit_surface_faces);
    assert(geometry.greedy.payload_bytes < geometry.reference.payload_bytes);
    assert(geometry.snapshot_allocated_bytes >= geometry.snapshot_payload_bytes);

    constexpr std::array operations{
        benchmark::VoxelMeshingOperation::snapshot_rebuild,
        benchmark::VoxelMeshingOperation::reference_mesh,
        benchmark::VoxelMeshingOperation::greedy_mesh_fresh,
        benchmark::VoxelMeshingOperation::greedy_mesh_reuse,
    };
    for (const auto operation : operations) {
        const auto* first = find_sample(report.value(), operation, 0);
        const auto* second = find_sample(report.value(), operation, 1);
        assert(first != nullptr);
        assert(second != nullptr);
        assert(first->work_items == 32U * 32U * 32U);
        assert(first->checksum == second->checksum);
    }
    assert(find_sample(report.value(), benchmark::VoxelMeshingOperation::greedy_mesh_fresh, 0)
               ->checksum ==
           find_sample(report.value(), benchmark::VoxelMeshingOperation::greedy_mesh_reuse, 0)
               ->checksum);

    const auto json = report.value().to_json();
    assert(json.contains("\"schema_version\": 1"));
    assert(json.contains("\"benchmark\": \"voxel_meshing\""));
    assert(json.contains("\"geometry\""));
    assert(json.contains("\"raw_samples\""));
    assert(json.contains("\"p95_nanoseconds\""));

    const std::filesystem::path output_path{"voxel_meshing_benchmark_test_output.json"};
    std::error_code error;
    std::filesystem::remove(output_path, error);
    assert(report.value().write_json(output_path));
    std::ifstream input(output_path, std::ios::binary);
    const std::string persisted{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
    assert(persisted == json);
    input.close();
    assert(std::filesystem::remove(output_path));
}

void test_invalid_configs_fail_closed() {
    benchmark::VoxelMeshingBenchmarkConfig config;
    config.corpora.clear();
    assert(!config.validate());
    config.corpora = {benchmark::VoxelCorpusKind::empty, benchmark::VoxelCorpusKind::empty};
    assert(!config.validate());
    config.corpora = {benchmark::VoxelCorpusKind::empty};
    config.repetitions = 0;
    assert(!config.validate());
    config.repetitions = 1;
    config.warmup_repetitions = 101;
    assert(!config.validate());
    config.warmup_repetitions = 0;
    config.material_count = 0;
    assert(!config.validate());
    assert(!benchmark::run_voxel_meshing_benchmark(config));
    assert(benchmark::voxel_meshing_operation_name(
               static_cast<benchmark::VoxelMeshingOperation>(255)) == "unknown");
}

} // namespace

int main() {
    test_small_benchmark_retains_raw_samples_and_geometry();
    test_invalid_configs_fail_closed();
    return 0;
}
