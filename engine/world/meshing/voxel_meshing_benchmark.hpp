#pragma once

#include "engine/core/result.hpp"
#include "engine/profiling/runtime_metadata.hpp"
#include "engine/world/voxels/voxel_storage_experiment.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::world::benchmark {

enum class VoxelMeshingOperation : std::uint8_t {
    snapshot_rebuild,
    reference_mesh,
    greedy_mesh_fresh,
    greedy_mesh_reuse,
};

[[nodiscard]] std::string_view
voxel_meshing_operation_name(VoxelMeshingOperation operation) noexcept;

struct VoxelMeshingBenchmarkConfig {
    std::vector<VoxelCorpusKind> corpora;
    std::uint16_t material_count = 32;
    std::uint64_t seed = 0x485354454144ULL;
    std::uint32_t warmup_repetitions = 3;
    std::uint32_t repetitions = 15;

    VoxelMeshingBenchmarkConfig();

    [[nodiscard]] core::Status validate() const;
};

struct VoxelMeshOutputMeasurement {
    std::size_t merged_face_count = 0;
    std::size_t unit_surface_face_count = 0;
    std::size_t vertex_count = 0;
    std::size_t index_count = 0;
    std::size_t section_count = 0;
    std::size_t payload_bytes = 0;
    std::size_t allocated_bytes = 0;
    std::array<std::size_t, 6> directional_unit_surface_faces{};
    std::uint64_t checksum = 0;
};

struct VoxelMeshingCorpusMeasurement {
    VoxelCorpusKind corpus = VoxelCorpusKind::empty;
    VoxelCorpusStats corpus_stats;
    std::size_t visible_unit_face_count = 0;
    std::size_t snapshot_cell_count = 0;
    std::size_t snapshot_payload_bytes = 0;
    std::size_t snapshot_allocated_bytes = 0;
    VoxelMeshOutputMeasurement reference;
    VoxelMeshOutputMeasurement greedy;
};

struct VoxelMeshingBenchmarkSample {
    VoxelCorpusKind corpus = VoxelCorpusKind::empty;
    VoxelMeshingOperation operation = VoxelMeshingOperation::snapshot_rebuild;
    std::uint32_t repetition = 0;
    std::uint32_t work_items = 0;
    std::uint64_t elapsed_nanoseconds = 0;
    std::uint64_t checksum = 0;

    [[nodiscard]] double nanoseconds_per_work_item() const noexcept;
};

struct VoxelMeshingBenchmarkSummary {
    VoxelCorpusKind corpus = VoxelCorpusKind::empty;
    VoxelMeshingOperation operation = VoxelMeshingOperation::snapshot_rebuild;
    std::size_t sample_count = 0;
    std::uint32_t work_items = 0;
    double minimum_nanoseconds = 0.0;
    double median_nanoseconds = 0.0;
    double p95_nanoseconds = 0.0;
    double maximum_nanoseconds = 0.0;
    double mean_nanoseconds = 0.0;
    double standard_deviation_nanoseconds = 0.0;
    double coefficient_of_variation = 0.0;
    double median_nanoseconds_per_work_item = 0.0;
};

struct VoxelMeshingBenchmarkReport {
    static constexpr std::uint32_t schema_version = 1;

    VoxelMeshingBenchmarkConfig config;
    profiling::RuntimeMetadata runtime;
    std::vector<VoxelMeshingCorpusMeasurement> geometry;
    std::vector<VoxelMeshingBenchmarkSample> raw_samples;

    [[nodiscard]] core::Status validate() const;
    [[nodiscard]] std::vector<VoxelMeshingBenchmarkSummary> summaries() const;
    [[nodiscard]] std::string to_json() const;
    [[nodiscard]] core::Status write_json(const std::filesystem::path& path) const;
};

[[nodiscard]] core::Result<VoxelMeshingBenchmarkReport>
run_voxel_meshing_benchmark(const VoxelMeshingBenchmarkConfig& config);

} // namespace heartstead::world::benchmark
