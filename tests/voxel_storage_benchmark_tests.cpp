#include "engine/world/voxels/voxel_storage_benchmark.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace benchmark = heartstead::world::benchmark;

[[nodiscard]] const benchmark::VoxelStorageBenchmarkSample*
find_sample(const benchmark::VoxelStorageBenchmarkReport& report,
            benchmark::VoxelStorageLayout layout, benchmark::VoxelStorageOperation operation,
            std::uint32_t repetition) {
    const auto found = std::ranges::find_if(report.raw_samples, [&](const auto& sample) {
        return sample.layout == layout && sample.operation == operation &&
               sample.repetition == repetition;
    });
    return found == report.raw_samples.end() ? nullptr : &*found;
}

void test_small_benchmark_retains_raw_samples_and_memory() {
    benchmark::VoxelStorageBenchmarkConfig config;
    config.corpora = {benchmark::VoxelCorpusKind::uniform_solid};
    config.edge_lengths = {16};
    config.warmup_repetitions = 0;
    config.repetitions = 2;
    config.iterations = 1;
    config.random_edits_per_iteration = 16;
    config.palette_growth_edits = 8;
    auto report = benchmark::run_voxel_storage_benchmark(config);
    assert(report);
    assert(report.value().validate());
    assert(report.value().memory.size() == 1);
    assert(report.value().raw_samples.size() == 44);
    assert(report.value().summaries().size() == 22);

    const auto& memory = report.value().memory.front();
    assert(memory.corpus_stats.cell_count == 4'096);
    assert(memory.corpus_stats.non_air_count == 4'096);
    assert(memory.visible_face_count == 6U * 16U * 16U);
    assert(memory.layouts.size() == 3);
    assert(memory.layouts[0].layout == benchmark::VoxelStorageLayout::dense);
    assert(memory.layouts[1].layout == benchmark::VoxelStorageLayout::split);
    assert(memory.layouts[2].layout == benchmark::VoxelStorageLayout::palette_packed);
    assert(memory.layouts[2].storage.palette_size == 1);
    assert(memory.layouts[2].storage.bits_per_index == 0);
    assert(memory.layouts[2].storage.payload_bytes < memory.layouts[0].storage.payload_bytes);

    constexpr std::array comparable_operations{
        benchmark::VoxelStorageOperation::type_scan,
        benchmark::VoxelStorageOperation::random_read,
        benchmark::VoxelStorageOperation::random_edit_existing,
        benchmark::VoxelStorageOperation::decode,
        benchmark::VoxelStorageOperation::serialize,
        benchmark::VoxelStorageOperation::face_mask_build,
    };
    for (const auto operation : comparable_operations) {
        const auto* dense =
            find_sample(report.value(), benchmark::VoxelStorageLayout::dense, operation, 0);
        const auto* split =
            find_sample(report.value(), benchmark::VoxelStorageLayout::split, operation, 0);
        const auto* packed = find_sample(
            report.value(), benchmark::VoxelStorageLayout::palette_packed, operation, 0);
        assert(dense != nullptr);
        assert(split != nullptr);
        assert(packed != nullptr);
        assert(dense->checksum == split->checksum);
        assert(dense->checksum == packed->checksum);
        assert(dense->iteration_count > 0);
        assert(dense->work_items_per_iteration > 0);
    }

    const auto json = report.value().to_json();
    assert(json.contains("\"schema_version\": 1"));
    assert(json.contains("\"benchmark\": \"voxel_storage\""));
    assert(json.contains("\"raw_samples\""));
    assert(json.contains("\"summaries\""));
    assert(json.contains("\"dense_blocks\""));
    assert(json.contains("\"allocated_plus_face_masks_bytes_per_visible_face\""));

    const std::filesystem::path output_path{"voxel_storage_benchmark_test_output.json"};
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
    benchmark::VoxelStorageBenchmarkConfig config;
    config.corpora = {benchmark::VoxelCorpusKind::empty, benchmark::VoxelCorpusKind::empty};
    assert(!config.validate());
    config.corpora = {benchmark::VoxelCorpusKind::empty};
    config.edge_lengths = {8};
    assert(!config.validate());
    config.edge_lengths = {16};
    config.repetitions = 0;
    assert(!config.validate());
    config.repetitions = 1;
    config.iterations = 0;
    assert(!config.validate());
    config.iterations = 1;
    config.random_edits_per_iteration = 4'097;
    assert(!config.validate());
    assert(!benchmark::run_voxel_storage_benchmark(config));

    assert(benchmark::voxel_storage_layout_name(static_cast<benchmark::VoxelStorageLayout>(255)) ==
           "unknown");
    assert(benchmark::voxel_storage_operation_name(
               static_cast<benchmark::VoxelStorageOperation>(255)) == "unknown");
}

} // namespace

int main() {
    test_small_benchmark_retains_raw_samples_and_memory();
    test_invalid_configs_fail_closed();
    return 0;
}
