#include "engine/world/meshing/voxel_meshing_benchmark.hpp"

#include "engine/world/chunks/chunk_database.hpp"
#include "engine/world/meshing/chunk_mesh_snapshot.hpp"
#include "engine/world/meshing/chunk_mesher.hpp"
#include "engine/world/meshing/greedy_chunk_mesher.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace heartstead::world::benchmark {

namespace {

constexpr std::array all_corpus_kinds{
    VoxelCorpusKind::empty,           VoxelCorpusKind::uniform_solid,
    VoxelCorpusKind::layered_terrain, VoxelCorpusKind::sparse_caves,
    VoxelCorpusKind::lit_settlement,  VoxelCorpusKind::checkerboard,
    VoxelCorpusKind::high_entropy,
};

constexpr std::array all_operations{
    VoxelMeshingOperation::snapshot_rebuild,
    VoxelMeshingOperation::reference_mesh,
    VoxelMeshingOperation::greedy_mesh_fresh,
    VoxelMeshingOperation::greedy_mesh_reuse,
};

constexpr std::uint64_t checksum_prime = 1'099'511'628'211ULL;

[[nodiscard]] std::uint64_t combine_checksum(std::uint64_t current, std::uint64_t value) noexcept {
    return (current * checksum_prime) ^ value;
}

[[nodiscard]] std::size_t linear_index(std::uint16_t x, std::uint16_t y, std::uint16_t z) noexcept {
    constexpr auto edge = static_cast<std::size_t>(VoxelChunk::edge_length);
    return static_cast<std::size_t>(z) * edge * edge + static_cast<std::size_t>(y) * edge + x;
}

[[nodiscard]] std::size_t visible_unit_faces(std::span<const VoxelCell> cells) noexcept {
    constexpr auto edge = static_cast<std::int32_t>(VoxelChunk::edge_length);
    constexpr std::array directions{
        std::array{-1, 0, 0}, std::array{1, 0, 0},  std::array{0, -1, 0},
        std::array{0, 1, 0},  std::array{0, 0, -1}, std::array{0, 0, 1},
    };
    std::size_t result = 0;
    for (std::int32_t z = 0; z < edge; ++z) {
        for (std::int32_t y = 0; y < edge; ++y) {
            for (std::int32_t x = 0; x < edge; ++x) {
                const auto index =
                    linear_index(static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y),
                                 static_cast<std::uint16_t>(z));
                if (cells[index].is_air()) {
                    continue;
                }
                for (const auto direction : directions) {
                    const auto neighbor_x = x + direction[0];
                    const auto neighbor_y = y + direction[1];
                    const auto neighbor_z = z + direction[2];
                    if (neighbor_x < 0 || neighbor_x >= edge || neighbor_y < 0 ||
                        neighbor_y >= edge || neighbor_z < 0 || neighbor_z >= edge ||
                        cells[linear_index(static_cast<std::uint16_t>(neighbor_x),
                                           static_cast<std::uint16_t>(neighbor_y),
                                           static_cast<std::uint16_t>(neighbor_z))]
                            .is_air()) {
                        ++result;
                    }
                }
            }
        }
    }
    return result;
}

[[nodiscard]] std::size_t direction_index(math::Vec3f normal) noexcept {
    if (normal.x < -0.5F) {
        return 0;
    }
    if (normal.x > 0.5F) {
        return 1;
    }
    if (normal.y < -0.5F) {
        return 2;
    }
    if (normal.y > 0.5F) {
        return 3;
    }
    if (normal.z < -0.5F) {
        return 4;
    }
    return 5;
}

[[nodiscard]] double triangle_area(math::Vec3f first, math::Vec3f second,
                                   math::Vec3f third) noexcept {
    const auto left = second - first;
    const auto right = third - first;
    const math::Vec3f cross{left.y * right.z - left.z * right.y,
                            left.z * right.x - left.x * right.z,
                            left.x * right.y - left.y * right.x};
    return 0.5 * std::sqrt(static_cast<double>(cross.x * cross.x + cross.y * cross.y +
                                               cross.z * cross.z));
}

[[nodiscard]] std::uint64_t cell_checksum(std::uint64_t current, VoxelCell cell) noexcept {
    current = combine_checksum(current, cell.type);
    current = combine_checksum(current, cell.light);
    current = combine_checksum(current, cell.state_bits);
    return combine_checksum(current, cell.metadata_handle);
}

[[nodiscard]] std::uint64_t snapshot_checksum(const ChunkNeighborhoodSnapshot& snapshot) noexcept {
    auto result = combine_checksum(0, snapshot.center_revision);
    result = combine_checksum(result, snapshot.side_length);
    result = combine_checksum(result, snapshot.center_occupancy.content_revision());
    result = combine_checksum(result, snapshot.center_occupancy.occupied_count());
    for (const auto word : snapshot.center_occupancy.words()) {
        result = combine_checksum(result, word);
    }
    result = combine_checksum(result, snapshot.meshing_masks.center_revision);
    result = combine_checksum(result, snapshot.meshing_masks.render_table_revision);
    result = combine_checksum(result, snapshot.meshing_masks.greedy_cube_count);
    result = combine_checksum(result, snapshot.meshing_masks.greedy_minimum.x);
    result = combine_checksum(result, snapshot.meshing_masks.greedy_minimum.y);
    result = combine_checksum(result, snapshot.meshing_masks.greedy_minimum.z);
    result = combine_checksum(result, snapshot.meshing_masks.greedy_maximum.x);
    result = combine_checksum(result, snapshot.meshing_masks.greedy_maximum.y);
    result = combine_checksum(result, snapshot.meshing_masks.greedy_maximum.z);
    result = combine_checksum(result, snapshot.meshing_masks.has_directional_occluders ? 1U : 0U);
    for (const auto word : snapshot.meshing_masks.words) {
        result = combine_checksum(result, word);
    }
    for (const auto cell : snapshot.cells) {
        result = cell_checksum(result, cell);
    }
    for (const auto& dependency : snapshot.dependencies) {
        result = combine_checksum(result, static_cast<std::uint64_t>(dependency.coordinate.x));
        result = combine_checksum(result, static_cast<std::uint64_t>(dependency.coordinate.y));
        result = combine_checksum(result, static_cast<std::uint64_t>(dependency.coordinate.z));
        result = combine_checksum(result, dependency.identity.load_generation);
        result = combine_checksum(result, dependency.content_revision);
        result = combine_checksum(result, dependency.present ? 1U : 0U);
    }
    return result;
}

[[nodiscard]] std::uint64_t mesh_checksum(const ChunkMesh& mesh) noexcept {
    auto result = combine_checksum(0, mesh.face_count);
    result = combine_checksum(result, mesh.triangle_face_count);
    for (const auto& vertex : mesh.vertices) {
        result = combine_checksum(result, std::bit_cast<std::uint32_t>(vertex.position.x));
        result = combine_checksum(result, std::bit_cast<std::uint32_t>(vertex.position.y));
        result = combine_checksum(result, std::bit_cast<std::uint32_t>(vertex.position.z));
        result = combine_checksum(result, std::bit_cast<std::uint32_t>(vertex.normal.x));
        result = combine_checksum(result, std::bit_cast<std::uint32_t>(vertex.normal.y));
        result = combine_checksum(result, std::bit_cast<std::uint32_t>(vertex.normal.z));
        result = combine_checksum(result, std::bit_cast<std::uint32_t>(vertex.u));
        result = combine_checksum(result, std::bit_cast<std::uint32_t>(vertex.v));
        result = combine_checksum(result, vertex.voxel_type);
        result = combine_checksum(result, vertex.light);
        result = combine_checksum(result, vertex.state_bits);
        result = combine_checksum(result, vertex.ambient_occlusion);
    }
    for (const auto index : mesh.indices) {
        result = combine_checksum(result, index);
    }
    for (const auto& section : mesh.sections) {
        result = combine_checksum(result, section.material_index);
        result = combine_checksum(result, static_cast<std::uint8_t>(section.render_phase));
        result = combine_checksum(result, section.first_index);
        result = combine_checksum(result, section.index_count);
    }
    return result;
}

[[nodiscard]] VoxelMeshOutputMeasurement measure_mesh_output(const ChunkMesh& mesh) {
    VoxelMeshOutputMeasurement result;
    result.merged_face_count = mesh.face_count;
    result.vertex_count = mesh.vertices.size();
    result.index_count = mesh.indices.size();
    result.section_count = mesh.sections.size();
    result.payload_bytes = mesh.vertices.size() * sizeof(ChunkMeshVertex) +
                           mesh.indices.size() * sizeof(std::uint32_t) +
                           mesh.sections.size() * sizeof(ChunkMeshSection) +
                           mesh.rich_instances.size() * sizeof(RichBlockMeshInstance);
    result.allocated_bytes = sizeof(mesh) + mesh.vertices.capacity() * sizeof(ChunkMeshVertex) +
                             mesh.indices.capacity() * sizeof(std::uint32_t) +
                             mesh.sections.capacity() * sizeof(ChunkMeshSection) +
                             mesh.rich_instances.capacity() * sizeof(RichBlockMeshInstance);
    std::array<double, 6> areas{};
    for (std::size_t index = 0; index + 2U < mesh.indices.size(); index += 3U) {
        const auto& first = mesh.vertices[mesh.indices[index]];
        const auto& second = mesh.vertices[mesh.indices[index + 1U]];
        const auto& third = mesh.vertices[mesh.indices[index + 2U]];
        areas[direction_index(first.normal)] +=
            triangle_area(first.position, second.position, third.position);
    }
    for (std::size_t direction = 0; direction < areas.size(); ++direction) {
        result.directional_unit_surface_faces[direction] =
            static_cast<std::size_t>(std::llround(areas[direction]));
        result.unit_surface_face_count += result.directional_unit_surface_faces[direction];
    }
    result.checksum = mesh_checksum(mesh);
    return result;
}

template <typename Function> [[nodiscard]] std::uint64_t measure_nanoseconds(Function&& function) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const auto started = std::chrono::steady_clock::now();
    function();
    const auto finished = std::chrono::steady_clock::now();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
    return elapsed <= 0 ? 0 : static_cast<std::uint64_t>(elapsed);
}

void append_sample(VoxelMeshingBenchmarkReport& report, VoxelCorpusKind corpus,
                   VoxelMeshingOperation operation, std::uint32_t repetition,
                   std::uint64_t elapsed_nanoseconds, std::uint64_t checksum) {
    report.raw_samples.push_back({corpus, operation, repetition,
                                  static_cast<std::uint32_t>(VoxelChunk::total_cells),
                                  elapsed_nanoseconds, checksum});
}

[[nodiscard]] core::Status collect_snapshot_samples(VoxelMeshingBenchmarkReport& report,
                                                    const VoxelMeshingBenchmarkConfig& config,
                                                    VoxelCorpusKind corpus,
                                                    const ChunkDatabase& chunks,
                                                    ChunkIdentity identity,
                                                    const BlockRenderTableSnapshot& render_table) {
    std::vector<VoxelCell> reusable_cells;
    std::vector<std::uint64_t> reusable_mask_words;
    const auto total_passes = config.warmup_repetitions + config.repetitions;
    for (std::uint32_t pass = 0; pass < total_passes; ++pass) {
        std::optional<ChunkNeighborhoodSnapshot> output;
        std::optional<core::Error> failure;
        const auto elapsed = measure_nanoseconds([&] {
            auto built = build_chunk_neighborhood_snapshot(chunks, identity, render_table,
                                                           std::move(reusable_cells),
                                                           std::move(reusable_mask_words));
            if (!built) {
                failure = built.error();
                return;
            }
            output = std::move(built).value();
        });
        if (failure) {
            return core::Status::failure(failure->code, failure->message);
        }
        if (!output) {
            return core::Status::failure("voxel_meshing.missing_snapshot_output",
                                         "snapshot benchmark did not retain its output");
        }
        const auto checksum = snapshot_checksum(*output);
        reusable_cells = std::move(output->cells);
        reusable_mask_words = std::move(output->meshing_masks.words);
        if (pass >= config.warmup_repetitions) {
            append_sample(report, corpus, VoxelMeshingOperation::snapshot_rebuild,
                          pass - config.warmup_repetitions, elapsed, checksum);
        }
    }
    return core::Status::ok();
}

[[nodiscard]] core::Status collect_mesh_samples(VoxelMeshingBenchmarkReport& report,
                                                const VoxelMeshingBenchmarkConfig& config,
                                                VoxelCorpusKind corpus,
                                                VoxelMeshingOperation operation,
                                                const ChunkNeighborhoodSnapshot& snapshot,
                                                const BlockRenderTableSnapshot& render_table) {
    ChunkMesh reusable_mesh;
    const auto total_passes = config.warmup_repetitions + config.repetitions;
    for (std::uint32_t pass = 0; pass < total_passes; ++pass) {
        std::optional<ChunkMesh> output;
        std::optional<core::Error> failure;
        auto input_mesh = std::move(reusable_mesh);
        const auto elapsed = measure_nanoseconds([&] {
            auto built = [&] {
                switch (operation) {
                case VoxelMeshingOperation::reference_mesh:
                    return ChunkMesher::build_surface_mesh(snapshot, render_table);
                case VoxelMeshingOperation::greedy_mesh_fresh:
                    return GreedyChunkMesher::build_surface_mesh(snapshot, render_table);
                case VoxelMeshingOperation::greedy_mesh_reuse:
                    return GreedyChunkMesher::build_surface_mesh(snapshot, render_table,
                                                                 std::move(input_mesh));
                case VoxelMeshingOperation::snapshot_rebuild:
                    break;
                }
                return core::Result<ChunkMesh>::failure(
                    "voxel_meshing.invalid_mesh_operation",
                    "snapshot rebuild cannot execute through the mesh collector");
            }();
            if (!built) {
                failure = built.error();
                return;
            }
            output = std::move(built).value();
        });
        if (failure) {
            return core::Status::failure(failure->code, failure->message);
        }
        if (!output) {
            return core::Status::failure("voxel_meshing.missing_mesh_output",
                                         "mesh benchmark did not retain its output");
        }
        const auto checksum = mesh_checksum(*output);
        if (operation == VoxelMeshingOperation::greedy_mesh_reuse) {
            reusable_mesh = std::move(*output);
        }
        if (pass >= config.warmup_repetitions) {
            append_sample(report, corpus, operation, pass - config.warmup_repetitions, elapsed,
                          checksum);
        }
    }
    return core::Status::ok();
}

[[nodiscard]] double percentile(const std::vector<double>& sorted, double fraction) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto position = fraction * static_cast<double>(sorted.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const auto weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

[[nodiscard]] std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        switch (character) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += character;
            break;
        }
    }
    return result;
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"' << json_escape(value) << '"';
}

[[nodiscard]] std::string checksum_string(std::uint64_t checksum) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << checksum;
    return output.str();
}

void write_directional_faces(std::ostream& output, const std::array<std::size_t, 6>& values) {
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        output << (index == 0 ? "" : ", ") << values[index];
    }
    output << ']';
}

void write_mesh_measurement(std::ostream& output, const VoxelMeshOutputMeasurement& measurement,
                            std::string_view indentation) {
    output << indentation << "{\n"
           << indentation << "  \"merged_face_count\": " << measurement.merged_face_count << ",\n"
           << indentation
           << "  \"unit_surface_face_count\": " << measurement.unit_surface_face_count << ",\n"
           << indentation << "  \"vertex_count\": " << measurement.vertex_count << ",\n"
           << indentation << "  \"index_count\": " << measurement.index_count << ",\n"
           << indentation << "  \"section_count\": " << measurement.section_count << ",\n"
           << indentation << "  \"payload_bytes\": " << measurement.payload_bytes << ",\n"
           << indentation << "  \"allocated_bytes\": " << measurement.allocated_bytes << ",\n"
           << indentation << "  \"directional_unit_surface_faces\": ";
    write_directional_faces(output, measurement.directional_unit_surface_faces);
    output << ",\n" << indentation << "  \"checksum\": ";
    write_json_string(output, checksum_string(measurement.checksum));
    output << '\n' << indentation << '}';
}

} // namespace

std::string_view voxel_meshing_operation_name(VoxelMeshingOperation operation) noexcept {
    switch (operation) {
    case VoxelMeshingOperation::snapshot_rebuild:
        return "snapshot_rebuild";
    case VoxelMeshingOperation::reference_mesh:
        return "reference_mesh";
    case VoxelMeshingOperation::greedy_mesh_fresh:
        return "greedy_mesh_fresh";
    case VoxelMeshingOperation::greedy_mesh_reuse:
        return "greedy_mesh_reuse";
    }
    return "unknown";
}

VoxelMeshingBenchmarkConfig::VoxelMeshingBenchmarkConfig()
    : corpora(all_corpus_kinds.begin(), all_corpus_kinds.end()) {}

core::Status VoxelMeshingBenchmarkConfig::validate() const {
    if (corpora.empty() || corpora.size() > all_corpus_kinds.size() || repetitions == 0 ||
        repetitions > 100U || warmup_repetitions > 100U || material_count == 0 ||
        material_count == std::numeric_limits<std::uint16_t>::max()) {
        return core::Status::failure(
            "voxel_meshing.invalid_config",
            "meshing benchmark corpus, repetition, or material limits are invalid");
    }
    std::set<VoxelCorpusKind> unique;
    for (const auto corpus : corpora) {
        VoxelCorpusConfig corpus_config{corpus, VoxelChunk::edge_length, material_count, seed};
        if (!corpus_config.validate() || !unique.insert(corpus).second) {
            return core::Status::failure(
                "voxel_meshing.invalid_corpus",
                "meshing benchmark corpora must be valid and appear only once");
        }
    }
    return core::Status::ok();
}

double VoxelMeshingBenchmarkSample::nanoseconds_per_work_item() const noexcept {
    return work_items == 0
               ? 0.0
               : static_cast<double>(elapsed_nanoseconds) / static_cast<double>(work_items);
}

core::Status VoxelMeshingBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    if (geometry.size() != config.corpora.size()) {
        return core::Status::failure("voxel_meshing.invalid_geometry_matrix",
                                     "meshing geometry measurements are incomplete");
    }
    std::set<VoxelCorpusKind> measured_corpora;
    for (const auto& measurement : geometry) {
        if (!measured_corpora.insert(measurement.corpus).second ||
            std::ranges::find(config.corpora, measurement.corpus) == config.corpora.end() ||
            measurement.corpus_stats.cell_count != VoxelChunk::total_cells ||
            measurement.snapshot_cell_count < VoxelChunk::total_cells ||
            measurement.snapshot_allocated_bytes < measurement.snapshot_payload_bytes ||
            measurement.snapshot_meshing_mask_allocated_bytes <
                measurement.snapshot_meshing_mask_payload_bytes ||
            measurement.reference.allocated_bytes < measurement.reference.payload_bytes ||
            measurement.greedy.allocated_bytes < measurement.greedy.payload_bytes ||
            measurement.reference.unit_surface_face_count != measurement.visible_unit_face_count ||
            measurement.greedy.unit_surface_face_count != measurement.visible_unit_face_count ||
            measurement.reference.directional_unit_surface_faces !=
                measurement.greedy.directional_unit_surface_faces) {
            return core::Status::failure(
                "voxel_meshing.invalid_geometry_measurement",
                "meshing output memory or reference/greedy surface parity is invalid");
        }
    }
    const auto expected_samples = config.corpora.size() * all_operations.size() *
                                  static_cast<std::size_t>(config.repetitions);
    if (raw_samples.size() != expected_samples) {
        return core::Status::failure("voxel_meshing.invalid_sample_matrix",
                                     "meshing raw sample matrix is incomplete");
    }
    for (const auto corpus : config.corpora) {
        std::optional<std::uint64_t> fresh_greedy_checksum;
        std::optional<std::uint64_t> reused_greedy_checksum;
        for (const auto operation : all_operations) {
            std::optional<std::uint64_t> expected_checksum;
            std::size_t count = 0;
            for (const auto& sample : raw_samples) {
                if (sample.corpus != corpus || sample.operation != operation) {
                    continue;
                }
                if (sample.repetition >= config.repetitions ||
                    sample.work_items != VoxelChunk::total_cells) {
                    return core::Status::failure("voxel_meshing.invalid_sample",
                                                 "meshing raw sample metadata is invalid");
                }
                if (expected_checksum && *expected_checksum != sample.checksum) {
                    return core::Status::failure(
                        "voxel_meshing.nondeterministic_output",
                        "meshing operation produced different output across repetitions");
                }
                expected_checksum = sample.checksum;
                ++count;
            }
            if (count != config.repetitions || !expected_checksum) {
                return core::Status::failure("voxel_meshing.missing_operation_samples",
                                             "meshing operation sample set is incomplete");
            }
            if (operation == VoxelMeshingOperation::greedy_mesh_fresh) {
                fresh_greedy_checksum = expected_checksum;
            } else if (operation == VoxelMeshingOperation::greedy_mesh_reuse) {
                reused_greedy_checksum = expected_checksum;
            }
        }
        if (fresh_greedy_checksum != reused_greedy_checksum) {
            return core::Status::failure(
                "voxel_meshing.reuse_output_mismatch",
                "fresh and buffer-reusing greedy mesh output must be identical");
        }
    }
    return core::Status::ok();
}

std::vector<VoxelMeshingBenchmarkSummary> VoxelMeshingBenchmarkReport::summaries() const {
    std::vector<VoxelMeshingBenchmarkSummary> result;
    for (const auto& sample : raw_samples) {
        const auto found = std::ranges::find_if(result, [&sample](const auto& summary) {
            return summary.corpus == sample.corpus && summary.operation == sample.operation;
        });
        if (found == result.end()) {
            result.push_back({sample.corpus, sample.operation, 0, sample.work_items});
        }
    }
    for (auto& summary : result) {
        std::vector<double> values;
        for (const auto& sample : raw_samples) {
            if (sample.corpus == summary.corpus && sample.operation == summary.operation) {
                values.push_back(static_cast<double>(sample.elapsed_nanoseconds));
            }
        }
        std::ranges::sort(values);
        summary.sample_count = values.size();
        if (values.empty()) {
            continue;
        }
        summary.minimum_nanoseconds = values.front();
        summary.median_nanoseconds = percentile(values, 0.50);
        summary.p95_nanoseconds = percentile(values, 0.95);
        summary.maximum_nanoseconds = values.back();
        summary.mean_nanoseconds =
            std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
        double squared_difference_total = 0.0;
        for (const auto value : values) {
            const auto difference = value - summary.mean_nanoseconds;
            squared_difference_total += difference * difference;
        }
        summary.standard_deviation_nanoseconds =
            std::sqrt(squared_difference_total / static_cast<double>(values.size()));
        summary.coefficient_of_variation =
            summary.mean_nanoseconds == 0.0
                ? 0.0
                : summary.standard_deviation_nanoseconds / summary.mean_nanoseconds;
        summary.median_nanoseconds_per_work_item =
            summary.median_nanoseconds / static_cast<double>(summary.work_items);
    }
    return result;
}

std::string VoxelMeshingBenchmarkReport::to_json() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n  \"schema_version\": " << schema_version
           << ",\n  \"benchmark\": \"voxel_meshing\",\n  \"runtime\": {\n";
    const auto write_runtime_string = [&output](std::string_view name, std::string_view value) {
        output << "    \"" << name << "\": ";
        write_json_string(output, value);
        output << ",\n";
    };
    write_runtime_string("engine_version", runtime.engine_version);
    write_runtime_string("git_commit", runtime.git_commit);
    write_runtime_string("build_configuration", runtime.build_configuration);
    write_runtime_string("compiler", runtime.compiler);
    write_runtime_string("platform", runtime.platform);
    write_runtime_string("architecture", runtime.architecture);
    write_runtime_string("operating_system", runtime.operating_system);
    write_runtime_string("cpu_model", runtime.cpu_model);
    output << "    \"logical_cpu_count\": " << runtime.logical_cpu_count
           << ",\n    \"git_dirty\": " << (runtime.git_dirty ? "true" : "false")
           << ",\n    \"tracy_enabled\": " << (runtime.tracy_enabled ? "true" : "false")
           << "\n  },\n  \"config\": {\n    \"seed\": " << config.seed
           << ",\n    \"edge_length\": " << VoxelChunk::edge_length
           << ",\n    \"material_count\": " << config.material_count
           << ",\n    \"warmup_repetitions\": " << config.warmup_repetitions
           << ",\n    \"repetitions\": " << config.repetitions << ",\n    \"corpora\": [";
    for (std::size_t index = 0; index < config.corpora.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        write_json_string(output, voxel_corpus_name(config.corpora[index]));
    }
    output << "]\n  },\n  \"geometry\": [\n";
    for (std::size_t index = 0; index < geometry.size(); ++index) {
        const auto& measurement = geometry[index];
        output << "    {\n      \"corpus\": ";
        write_json_string(output, voxel_corpus_name(measurement.corpus));
        output << ",\n      \"cell_count\": " << measurement.corpus_stats.cell_count
               << ",\n      \"non_air_count\": " << measurement.corpus_stats.non_air_count
               << ",\n      \"unique_cell_count\": " << measurement.corpus_stats.unique_cell_count
               << ",\n      \"unique_block_value_count\": "
               << measurement.corpus_stats.unique_block_value_count
               << ",\n      \"metadata_cell_count\": "
               << measurement.corpus_stats.metadata_cell_count
               << ",\n      \"visible_unit_face_count\": " << measurement.visible_unit_face_count
               << ",\n      \"snapshot_cell_count\": " << measurement.snapshot_cell_count
               << ",\n      \"snapshot_payload_bytes\": " << measurement.snapshot_payload_bytes
               << ",\n      \"snapshot_allocated_bytes\": " << measurement.snapshot_allocated_bytes
               << ",\n      \"snapshot_meshing_mask_payload_bytes\": "
               << measurement.snapshot_meshing_mask_payload_bytes
               << ",\n      \"snapshot_meshing_mask_allocated_bytes\": "
               << measurement.snapshot_meshing_mask_allocated_bytes << ",\n      \"reference\": ";
        write_mesh_measurement(output, measurement.reference, "      ");
        output << ",\n      \"greedy\": ";
        write_mesh_measurement(output, measurement.greedy, "      ");
        output << "\n    }" << (index + 1U == geometry.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"raw_samples\": [\n";
    for (std::size_t index = 0; index < raw_samples.size(); ++index) {
        const auto& sample = raw_samples[index];
        output << "    {\"corpus\": ";
        write_json_string(output, voxel_corpus_name(sample.corpus));
        output << ", \"operation\": ";
        write_json_string(output, voxel_meshing_operation_name(sample.operation));
        output << ", \"repetition\": " << sample.repetition
               << ", \"work_items\": " << sample.work_items
               << ", \"elapsed_nanoseconds\": " << sample.elapsed_nanoseconds
               << ", \"nanoseconds_per_work_item\": " << sample.nanoseconds_per_work_item()
               << ", \"checksum\": ";
        write_json_string(output, checksum_string(sample.checksum));
        output << '}' << (index + 1U == raw_samples.size() ? "\n" : ",\n");
    }
    const auto summary_values = summaries();
    output << "  ],\n  \"summaries\": [\n";
    for (std::size_t index = 0; index < summary_values.size(); ++index) {
        const auto& summary = summary_values[index];
        output << "    {\"corpus\": ";
        write_json_string(output, voxel_corpus_name(summary.corpus));
        output << ", \"operation\": ";
        write_json_string(output, voxel_meshing_operation_name(summary.operation));
        output << ", \"sample_count\": " << summary.sample_count
               << ", \"work_items\": " << summary.work_items
               << ", \"minimum_nanoseconds\": " << summary.minimum_nanoseconds
               << ", \"median_nanoseconds\": " << summary.median_nanoseconds
               << ", \"p95_nanoseconds\": " << summary.p95_nanoseconds
               << ", \"maximum_nanoseconds\": " << summary.maximum_nanoseconds
               << ", \"mean_nanoseconds\": " << summary.mean_nanoseconds
               << ", \"standard_deviation_nanoseconds\": " << summary.standard_deviation_nanoseconds
               << ", \"coefficient_of_variation\": " << summary.coefficient_of_variation
               << ", \"median_nanoseconds_per_work_item\": "
               << summary.median_nanoseconds_per_work_item << '}'
               << (index + 1U == summary_values.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

core::Status VoxelMeshingBenchmarkReport::write_json(const std::filesystem::path& path) const {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return core::Status::failure("voxel_meshing.create_directory_failed",
                                         "failed to create meshing benchmark output directory: " +
                                             error.message());
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return core::Status::failure("voxel_meshing.open_output_failed",
                                     "failed to open meshing benchmark output: " + path.string());
    }
    const auto json = to_json();
    stream.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!stream) {
        return core::Status::failure("voxel_meshing.write_output_failed",
                                     "failed to write meshing benchmark output: " + path.string());
    }
    return core::Status::ok();
}

core::Result<VoxelMeshingBenchmarkReport>
run_voxel_meshing_benchmark(const VoxelMeshingBenchmarkConfig& config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<VoxelMeshingBenchmarkReport>::failure(status.error().code,
                                                                  status.error().message);
    }
    auto render_table = build_block_render_table_snapshot(nullptr);
    if (!render_table) {
        return core::Result<VoxelMeshingBenchmarkReport>::failure(render_table.error().code,
                                                                  render_table.error().message);
    }

    VoxelMeshingBenchmarkReport report;
    report.config = config;
    report.runtime = profiling::query_runtime_metadata();
    for (const auto corpus_kind : config.corpora) {
        auto generated = generate_voxel_storage_corpus(
            {corpus_kind, VoxelChunk::edge_length, config.material_count, config.seed});
        if (!generated) {
            return core::Result<VoxelMeshingBenchmarkReport>::failure(generated.error().code,
                                                                      generated.error().message);
        }
        auto corpus = std::move(generated).value();
        const auto corpus_stats = corpus.stats();
        const auto expected_faces = visible_unit_faces(corpus.cells);

        ChunkDatabase chunks;
        auto& chunk = chunks.get_or_create({0, 0, 0});
        status = chunk.load_generated_cells(std::move(corpus.cells));
        if (!status) {
            return core::Result<VoxelMeshingBenchmarkReport>::failure(status.error().code,
                                                                      status.error().message);
        }
        auto snapshot =
            build_chunk_neighborhood_snapshot(chunks, chunk.identity(), render_table.value());
        if (!snapshot) {
            return core::Result<VoxelMeshingBenchmarkReport>::failure(snapshot.error().code,
                                                                      snapshot.error().message);
        }
        auto reference = ChunkMesher::build_surface_mesh(snapshot.value(), render_table.value());
        auto greedy = GreedyChunkMesher::build_surface_mesh(snapshot.value(), render_table.value());
        if (!reference || !greedy) {
            const auto& error = !reference ? reference.error() : greedy.error();
            return core::Result<VoxelMeshingBenchmarkReport>::failure(error.code, error.message);
        }

        VoxelMeshingCorpusMeasurement measurement;
        measurement.corpus = corpus_kind;
        measurement.corpus_stats = corpus_stats;
        measurement.visible_unit_face_count = expected_faces;
        measurement.snapshot_cell_count = snapshot.value().cells.size();
        measurement.snapshot_meshing_mask_payload_bytes =
            snapshot.value().meshing_masks.payload_bytes();
        measurement.snapshot_meshing_mask_allocated_bytes =
            snapshot.value().meshing_masks.allocated_bytes();
        measurement.snapshot_payload_bytes =
            snapshot.value().cells.size() * sizeof(VoxelCell) +
            snapshot.value().dependencies.size() * sizeof(ChunkDependencyRevision) +
            VoxelOccupancyMask::payload_bytes + measurement.snapshot_meshing_mask_payload_bytes;
        measurement.snapshot_allocated_bytes =
            sizeof(ChunkNeighborhoodSnapshot) +
            snapshot.value().cells.capacity() * sizeof(VoxelCell) +
            snapshot.value().dependencies.capacity() * sizeof(ChunkDependencyRevision) +
            measurement.snapshot_meshing_mask_allocated_bytes;
        measurement.reference = measure_mesh_output(reference.value());
        measurement.greedy = measure_mesh_output(greedy.value());
        report.geometry.push_back(std::move(measurement));

        status = collect_snapshot_samples(report, config, corpus_kind, chunks, chunk.identity(),
                                          render_table.value());
        if (status) {
            status = collect_mesh_samples(report, config, corpus_kind,
                                          VoxelMeshingOperation::reference_mesh, snapshot.value(),
                                          render_table.value());
        }
        if (status) {
            status = collect_mesh_samples(report, config, corpus_kind,
                                          VoxelMeshingOperation::greedy_mesh_fresh,
                                          snapshot.value(), render_table.value());
        }
        if (status) {
            status = collect_mesh_samples(report, config, corpus_kind,
                                          VoxelMeshingOperation::greedy_mesh_reuse,
                                          snapshot.value(), render_table.value());
        }
        if (!status) {
            return core::Result<VoxelMeshingBenchmarkReport>::failure(status.error().code,
                                                                      status.error().message);
        }
    }
    status = report.validate();
    if (!status) {
        return core::Result<VoxelMeshingBenchmarkReport>::failure(status.error().code,
                                                                  status.error().message);
    }
    return core::Result<VoxelMeshingBenchmarkReport>::success(std::move(report));
}

} // namespace heartstead::world::benchmark
