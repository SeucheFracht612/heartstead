#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/world/voxels/voxel_storage_experiment.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::world::benchmark {

enum class VoxelStorageLayout : std::uint8_t {
    dense,
    split,
    palette_packed,
};

[[nodiscard]] std::string_view voxel_storage_layout_name(VoxelStorageLayout layout) noexcept;

enum class VoxelStorageOperation : std::uint8_t {
    type_scan,
    random_read,
    random_edit_existing,
    encode,
    decode,
    serialize,
    face_mask_build,
    palette_growth_edit,
};

[[nodiscard]] std::string_view
voxel_storage_operation_name(VoxelStorageOperation operation) noexcept;

struct VoxelStorageBenchmarkConfig {
    std::vector<VoxelCorpusKind> corpora;
    std::vector<std::uint16_t> edge_lengths;
    std::uint16_t material_count = 32;
    std::uint64_t seed = 0x485354454144ULL;
    std::uint32_t warmup_repetitions = 2;
    std::uint32_t repetitions = 9;
    std::uint32_t iterations = 8;
    std::uint32_t random_edits_per_iteration = 256;
    std::uint32_t palette_growth_edits = 64;

    VoxelStorageBenchmarkConfig();

    [[nodiscard]] core::Status validate() const;
};

struct VoxelStorageMemoryMeasurement {
    VoxelStorageLayout layout = VoxelStorageLayout::dense;
    VoxelSectionStorageStats storage;
};

struct VoxelCorpusMemoryMeasurement {
    VoxelCorpusKind corpus = VoxelCorpusKind::empty;
    std::uint16_t edge_length = 0;
    VoxelCorpusStats corpus_stats;
    std::size_t visible_face_count = 0;
    std::size_t face_mask_payload_bytes = 0;
    std::vector<VoxelStorageMemoryMeasurement> layouts;
};

struct VoxelStorageBenchmarkSample {
    VoxelCorpusKind corpus = VoxelCorpusKind::empty;
    std::uint16_t edge_length = 0;
    VoxelStorageLayout layout = VoxelStorageLayout::dense;
    VoxelStorageOperation operation = VoxelStorageOperation::type_scan;
    std::uint32_t repetition = 0;
    std::uint32_t iteration_count = 0;
    std::uint32_t work_items_per_iteration = 0;
    std::uint64_t elapsed_nanoseconds = 0;
    std::uint64_t checksum = 0;

    [[nodiscard]] double nanoseconds_per_iteration() const noexcept;
    [[nodiscard]] double nanoseconds_per_work_item() const noexcept;
};

struct VoxelStorageBenchmarkSummary {
    VoxelCorpusKind corpus = VoxelCorpusKind::empty;
    std::uint16_t edge_length = 0;
    VoxelStorageLayout layout = VoxelStorageLayout::dense;
    VoxelStorageOperation operation = VoxelStorageOperation::type_scan;
    std::size_t sample_count = 0;
    std::uint32_t work_items_per_iteration = 0;
    double minimum_nanoseconds_per_iteration = 0.0;
    double median_nanoseconds_per_iteration = 0.0;
    double p95_nanoseconds_per_iteration = 0.0;
    double maximum_nanoseconds_per_iteration = 0.0;
    double mean_nanoseconds_per_iteration = 0.0;
    double standard_deviation_nanoseconds = 0.0;
    double coefficient_of_variation = 0.0;
    double median_nanoseconds_per_work_item = 0.0;
};

struct VoxelStorageBenchmarkReport {
    static constexpr std::uint32_t schema_version = 1;

    VoxelStorageBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    std::vector<VoxelCorpusMemoryMeasurement> memory;
    std::vector<VoxelStorageBenchmarkSample> raw_samples;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] std::vector<VoxelStorageBenchmarkSummary> summaries() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<VoxelStorageBenchmarkReport>
run_voxel_storage_benchmark(const VoxelStorageBenchmarkConfig& config);

} // namespace heartstead::world::benchmark
