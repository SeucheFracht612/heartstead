#include "engine/world/voxels/voxel_storage_benchmark.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <type_traits>
#include <utility>

namespace heartstead::world::benchmark {

namespace {

constexpr std::array all_corpus_kinds{
    VoxelCorpusKind::empty,           VoxelCorpusKind::uniform_solid,
    VoxelCorpusKind::layered_terrain, VoxelCorpusKind::sparse_caves,
    VoxelCorpusKind::lit_settlement,  VoxelCorpusKind::checkerboard,
    VoxelCorpusKind::high_entropy,
};

constexpr std::uint64_t checksum_prime = 1'099'511'628'211ULL;

struct EditCommand {
    std::size_t index = 0;
    VoxelCell value;
};

struct FaceMasks {
    std::vector<std::uint64_t> occupied;
    std::array<std::vector<std::uint64_t>, 6> exposed;
    std::size_t occupied_count = 0;
    std::size_t visible_face_count = 0;

    [[nodiscard]] std::size_t payload_bytes() const noexcept {
        std::size_t result = occupied.size() * sizeof(occupied.front());
        for (const auto& face : exposed) {
            result += face.size() * sizeof(face.front());
        }
        return result;
    }

    [[nodiscard]] std::uint64_t checksum() const noexcept {
        auto result = static_cast<std::uint64_t>(occupied_count);
        result = (result * checksum_prime) ^ static_cast<std::uint64_t>(visible_face_count);
        for (const auto word : occupied) {
            result = (result * checksum_prime) ^ word;
        }
        for (const auto& face : exposed) {
            for (const auto word : face) {
                result = (result * checksum_prime) ^ word;
            }
        }
        return result;
    }

    friend bool operator==(const FaceMasks&, const FaceMasks&) = default;
};

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::size_t cell_count(std::uint16_t edge_length) noexcept {
    const auto edge = static_cast<std::size_t>(edge_length);
    return edge * edge * edge;
}

[[nodiscard]] std::size_t linear_index(std::uint16_t edge_length, std::uint16_t x, std::uint16_t y,
                                       std::uint16_t z) noexcept {
    const auto edge = static_cast<std::size_t>(edge_length);
    return static_cast<std::size_t>(z) * edge * edge + static_cast<std::size_t>(y) * edge + x;
}

void set_bit(std::span<std::uint64_t> words, std::size_t index) noexcept {
    words[index / 64U] |= std::uint64_t{1} << (index % 64U);
}

template <typename OccupancyQuery>
[[nodiscard]] FaceMasks build_face_masks(std::uint16_t edge_length, OccupancyQuery&& is_occupied) {
    const auto count = cell_count(edge_length);
    const auto word_count = (count + 63U) / 64U;
    FaceMasks result;
    result.occupied.resize(word_count);
    for (auto& face : result.exposed) {
        face.resize(word_count);
    }
    constexpr std::array directions{
        std::array{-1, 0, 0}, std::array{1, 0, 0},  std::array{0, -1, 0},
        std::array{0, 1, 0},  std::array{0, 0, -1}, std::array{0, 0, 1},
    };
    const auto edge = static_cast<int>(edge_length);
    for (int z = 0; z < edge; ++z) {
        for (int y = 0; y < edge; ++y) {
            for (int x = 0; x < edge; ++x) {
                const auto index =
                    linear_index(edge_length, static_cast<std::uint16_t>(x),
                                 static_cast<std::uint16_t>(y), static_cast<std::uint16_t>(z));
                if (!is_occupied(index)) {
                    continue;
                }
                ++result.occupied_count;
                set_bit(result.occupied, index);
                for (std::size_t direction = 0; direction < directions.size(); ++direction) {
                    const auto neighbor_x = x + directions[direction][0];
                    const auto neighbor_y = y + directions[direction][1];
                    const auto neighbor_z = z + directions[direction][2];
                    const bool boundary = neighbor_x < 0 || neighbor_x >= edge || neighbor_y < 0 ||
                                          neighbor_y >= edge || neighbor_z < 0 ||
                                          neighbor_z >= edge;
                    const auto exposed =
                        boundary || !is_occupied(linear_index(
                                        edge_length, static_cast<std::uint16_t>(neighbor_x),
                                        static_cast<std::uint16_t>(neighbor_y),
                                        static_cast<std::uint16_t>(neighbor_z)));
                    if (exposed) {
                        set_bit(result.exposed[direction], index);
                        ++result.visible_face_count;
                    }
                }
            }
        }
    }
    return result;
}

[[nodiscard]] std::uint64_t combine_checksum(std::uint64_t current, std::uint64_t value) noexcept {
    return (current * checksum_prime) ^ value;
}

template <typename Value> void observe_memory(std::span<const Value> values) noexcept {
#if defined(__clang__) || defined(__GNUC__)
    const auto* data = values.data();
    const auto bytes = values.size_bytes();
    __asm__ __volatile__("" : : "r"(data), "r"(bytes) : "memory");
#else
    static_cast<void>(values);
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

[[nodiscard]] std::uint64_t cell_checksum(std::uint64_t current, VoxelCell cell) noexcept {
    current = combine_checksum(current, cell.type);
    current = combine_checksum(current, cell.light);
    current = combine_checksum(current, cell.state_bits);
    return combine_checksum(current, cell.metadata_handle);
}

template <typename Accessor>
[[nodiscard]] std::uint64_t full_cell_checksum(std::size_t count, Accessor&& cell_at) noexcept {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < count; ++index) {
        result = cell_checksum(result, cell_at(index));
    }
    return result;
}

[[nodiscard]] std::uint64_t dense_type_checksum(std::span<const VoxelCell> cells) noexcept {
    std::uint64_t result = 0;
    for (const auto cell : cells) {
        result = combine_checksum(result, cell.type);
    }
    return result;
}

template <typename Accessor>
[[nodiscard]] std::uint64_t random_read_checksum(std::span<const std::size_t> indices,
                                                 Accessor&& cell_at) noexcept {
    std::uint64_t result = 0;
    for (const auto index : indices) {
        result = cell_checksum(result, cell_at(index));
    }
    return result;
}

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
}

template <typename Accessor>
[[nodiscard]] std::vector<std::uint8_t> serialize_canonical(std::size_t count, Accessor&& cell_at) {
    std::vector<std::uint8_t> output;
    output.reserve(count * 9U);
    for (std::size_t index = 0; index < count; ++index) {
        const auto cell = cell_at(index);
        append_u16(output, cell.type);
        output.push_back(cell.light);
        append_u16(output, cell.state_bits);
        append_u32(output, cell.metadata_handle);
    }
    return output;
}

[[nodiscard]] std::uint64_t byte_checksum(std::span<const std::uint8_t> bytes) noexcept {
    observe_memory(bytes);
    auto result = static_cast<std::uint64_t>(bytes.size());
    if (!bytes.empty()) {
        result = combine_checksum(result, bytes.front());
        result = combine_checksum(result, bytes.back());
    }
    return result;
}

[[nodiscard]] std::vector<std::size_t> random_indices(std::size_t count, std::uint64_t seed) {
    std::vector<std::size_t> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        result.push_back(static_cast<std::size_t>(splitmix64(seed + index) % count));
    }
    return result;
}

[[nodiscard]] std::vector<EditCommand> existing_palette_edits(const VoxelStorageCorpus& corpus,
                                                              std::size_t edit_count,
                                                              std::uint64_t seed) {
    std::vector<EditCommand> result;
    result.reserve(edit_count);
    for (std::size_t edit = 0; edit < edit_count; ++edit) {
        const auto random = splitmix64(seed + edit);
        const auto source_random = splitmix64(random);
        auto value = corpus.cells[static_cast<std::size_t>(source_random % corpus.cells.size())];
        value.light = static_cast<std::uint8_t>(random >> 40U);
        value.metadata_handle =
            edit % 13U == 0 ? static_cast<std::uint32_t>((random >> 24U) | std::uint64_t{1}) : 0;
        result.push_back({static_cast<std::size_t>(random % corpus.cells.size()), value});
    }
    return result;
}

[[nodiscard]] std::vector<EditCommand>
palette_growth_edits(std::size_t count, std::size_t section_cells, std::uint64_t seed) {
    std::vector<EditCommand> result;
    result.reserve(count);
    for (std::size_t edit = 0; edit < count; ++edit) {
        const auto random = splitmix64(seed + edit);
        VoxelCell value;
        value.type = 60'000;
        value.state_bits = static_cast<std::uint16_t>(edit + 1U);
        value.light = static_cast<std::uint8_t>(random >> 40U);
        value.metadata_handle =
            edit % 13U == 0 ? static_cast<std::uint32_t>((random >> 24U) | std::uint64_t{1}) : 0;
        result.push_back({static_cast<std::size_t>(random % section_cells), value});
    }
    return result;
}

[[nodiscard]] VoxelSectionStorageStats dense_storage_stats(const VoxelStorageCorpus& corpus) {
    VoxelSectionStorageStats result;
    result.cell_count = corpus.cells.size();
    result.payload_bytes = corpus.cells.size() * sizeof(VoxelCell);
    result.allocated_bytes = sizeof(corpus.cells) + corpus.cells.capacity() * sizeof(VoxelCell);
    result.light_bytes = corpus.cells.size() * sizeof(VoxelCell::light);
    result.metadata_entry_count = corpus.stats().metadata_cell_count;
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

void append_sample(VoxelStorageBenchmarkReport& report, VoxelCorpusKind corpus,
                   std::uint16_t edge_length, VoxelStorageLayout layout,
                   VoxelStorageOperation operation, std::uint32_t repetition,
                   std::uint32_t iteration_count, std::uint32_t work_items_per_iteration,
                   std::uint64_t elapsed_nanoseconds, std::uint64_t checksum) {
    report.raw_samples.push_back({corpus, edge_length, layout, operation, repetition,
                                  iteration_count, work_items_per_iteration, elapsed_nanoseconds,
                                  checksum});
}

template <typename Function>
void collect_read_only(VoxelStorageBenchmarkReport& report,
                       const VoxelStorageBenchmarkConfig& config, VoxelCorpusKind corpus,
                       std::uint16_t edge_length, VoxelStorageLayout layout,
                       VoxelStorageOperation operation, std::uint32_t work_items_per_iteration,
                       Function&& function) {
    const auto total_passes = config.warmup_repetitions + config.repetitions;
    for (std::uint32_t pass = 0; pass < total_passes; ++pass) {
        std::uint64_t checksum = 0;
        const auto elapsed = measure_nanoseconds([&] {
            for (std::uint32_t iteration = 0; iteration < config.iterations; ++iteration) {
                checksum = combine_checksum(checksum, function(iteration));
            }
        });
        if (pass >= config.warmup_repetitions) {
            append_sample(report, corpus, edge_length, layout, operation,
                          pass - config.warmup_repetitions, config.iterations,
                          work_items_per_iteration, elapsed, checksum);
        }
    }
}

template <typename Prepare, typename Execute, typename Finish>
void collect_stateful(VoxelStorageBenchmarkReport& report,
                      const VoxelStorageBenchmarkConfig& config, VoxelCorpusKind corpus,
                      std::uint16_t edge_length, VoxelStorageLayout layout,
                      VoxelStorageOperation operation, std::uint32_t iteration_count,
                      std::uint32_t work_items_per_iteration, Prepare&& prepare, Execute&& execute,
                      Finish&& finish) {
    const auto total_passes = config.warmup_repetitions + config.repetitions;
    for (std::uint32_t pass = 0; pass < total_passes; ++pass) {
        auto state = prepare();
        std::uint64_t rolling_checksum = 0;
        const auto elapsed = measure_nanoseconds([&] {
            for (std::uint32_t iteration = 0; iteration < iteration_count; ++iteration) {
                rolling_checksum = combine_checksum(rolling_checksum, execute(state, iteration));
            }
        });
        const auto checksum = combine_checksum(rolling_checksum, finish(state));
        if (pass >= config.warmup_repetitions) {
            append_sample(report, corpus, edge_length, layout, operation,
                          pass - config.warmup_repetitions, iteration_count,
                          work_items_per_iteration, elapsed, checksum);
        }
    }
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

void write_ratio(std::ostream& output, std::size_t numerator, std::size_t denominator) {
    if (denominator == 0) {
        output << "null";
        return;
    }
    output << static_cast<double>(numerator) / static_cast<double>(denominator);
}

[[nodiscard]] std::string checksum_string(std::uint64_t checksum) {
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(16) << std::setfill('0') << checksum;
    return output.str();
}

} // namespace

std::string_view voxel_storage_layout_name(VoxelStorageLayout layout) noexcept {
    switch (layout) {
    case VoxelStorageLayout::dense:
        return "dense";
    case VoxelStorageLayout::split:
        return "split";
    case VoxelStorageLayout::palette_packed:
        return "palette_packed";
    }
    return "unknown";
}

std::string_view voxel_storage_operation_name(VoxelStorageOperation operation) noexcept {
    switch (operation) {
    case VoxelStorageOperation::type_scan:
        return "type_scan";
    case VoxelStorageOperation::random_read:
        return "random_read";
    case VoxelStorageOperation::random_edit_existing:
        return "random_edit_existing";
    case VoxelStorageOperation::encode:
        return "encode";
    case VoxelStorageOperation::decode:
        return "decode";
    case VoxelStorageOperation::serialize:
        return "serialize";
    case VoxelStorageOperation::face_mask_build:
        return "face_mask_build";
    case VoxelStorageOperation::palette_growth_edit:
        return "palette_growth_edit";
    }
    return "unknown";
}

VoxelStorageBenchmarkConfig::VoxelStorageBenchmarkConfig()
    : corpora(all_corpus_kinds.begin(), all_corpus_kinds.end()), edge_lengths{16, 32} {}

core::Status VoxelStorageBenchmarkConfig::validate() const {
    if (corpora.empty() || corpora.size() > all_corpus_kinds.size()) {
        return core::Status::failure("voxel_benchmark.invalid_corpora",
                                     "voxel benchmark requires one to seven corpora");
    }
    std::set<VoxelCorpusKind> unique_corpora;
    for (const auto corpus : corpora) {
        if (!unique_corpora.insert(corpus).second) {
            return core::Status::failure("voxel_benchmark.duplicate_corpus",
                                         "voxel benchmark corpora must be unique");
        }
    }
    if (edge_lengths.empty() || edge_lengths.size() > 2U) {
        return core::Status::failure("voxel_benchmark.invalid_edges",
                                     "voxel benchmark requires edge length 16, 32, or both");
    }
    std::set<std::uint16_t> unique_edges;
    std::size_t minimum_cell_count = std::numeric_limits<std::size_t>::max();
    for (const auto edge_length : edge_lengths) {
        if ((edge_length != 16 && edge_length != 32) || !unique_edges.insert(edge_length).second) {
            return core::Status::failure(
                "voxel_benchmark.invalid_edge",
                "voxel benchmark edge lengths must be unique values selected from 16 and 32");
        }
        minimum_cell_count = std::min(minimum_cell_count, cell_count(edge_length));
    }
    for (const auto corpus : corpora) {
        for (const auto edge_length : edge_lengths) {
            const auto status =
                VoxelCorpusConfig{corpus, edge_length, material_count, seed}.validate();
            if (!status) {
                return status;
            }
        }
    }
    if (warmup_repetitions > 100U || repetitions == 0 || repetitions > 100U || iterations == 0 ||
        iterations > 10'000U) {
        return core::Status::failure(
            "voxel_benchmark.invalid_repetitions",
            "voxel benchmark warmup, repetitions, or iterations exceed safe limits");
    }
    if (random_edits_per_iteration == 0 || random_edits_per_iteration > minimum_cell_count ||
        palette_growth_edits == 0 || palette_growth_edits > minimum_cell_count) {
        return core::Status::failure("voxel_benchmark.invalid_edit_count",
                                     "voxel benchmark edit counts must fit every selected section");
    }
    return core::Status::ok();
}

double VoxelStorageBenchmarkSample::nanoseconds_per_iteration() const noexcept {
    return iteration_count == 0
               ? 0.0
               : static_cast<double>(elapsed_nanoseconds) / static_cast<double>(iteration_count);
}

double VoxelStorageBenchmarkSample::nanoseconds_per_work_item() const noexcept {
    if (iteration_count == 0 || work_items_per_iteration == 0) {
        return 0.0;
    }
    return static_cast<double>(elapsed_nanoseconds) / static_cast<double>(iteration_count) /
           static_cast<double>(work_items_per_iteration);
}

core::Status VoxelStorageBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    const auto expected_corpora = config.corpora.size() * config.edge_lengths.size();
    if (memory.size() != expected_corpora) {
        return core::Status::failure("voxel_benchmark.invalid_memory_measurements",
                                     "voxel benchmark memory matrix is incomplete");
    }
    for (const auto& measurement : memory) {
        if (measurement.edge_length == 0 || measurement.corpus_stats.cell_count == 0 ||
            measurement.layouts.size() != 3U) {
            return core::Status::failure("voxel_benchmark.invalid_memory_measurement",
                                         "voxel benchmark memory measurement is incomplete");
        }
        std::set<VoxelStorageLayout> layouts;
        for (const auto& layout : measurement.layouts) {
            if (!layouts.insert(layout.layout).second ||
                layout.storage.cell_count != measurement.corpus_stats.cell_count ||
                layout.storage.allocated_bytes < layout.storage.payload_bytes) {
                return core::Status::failure(
                    "voxel_benchmark.invalid_layout_measurement",
                    "voxel benchmark layout bytes or identity are inconsistent");
            }
        }
    }
    constexpr std::size_t operations_per_corpus = 22;
    const auto expected_samples = expected_corpora * operations_per_corpus * config.repetitions;
    if (raw_samples.size() != expected_samples) {
        return core::Status::failure("voxel_benchmark.invalid_raw_samples",
                                     "voxel benchmark raw sample matrix is incomplete");
    }
    for (const auto& sample : raw_samples) {
        if (sample.iteration_count == 0 || sample.work_items_per_iteration == 0 ||
            sample.repetition >= config.repetitions) {
            return core::Status::failure("voxel_benchmark.invalid_sample",
                                         "voxel benchmark raw sample is invalid");
        }
    }
    return core::Status::ok();
}

std::vector<VoxelStorageBenchmarkSummary> VoxelStorageBenchmarkReport::summaries() const {
    std::vector<VoxelStorageBenchmarkSummary> result;
    for (const auto& sample : raw_samples) {
        const auto found = std::ranges::find_if(result, [&sample](const auto& summary) {
            return summary.corpus == sample.corpus && summary.edge_length == sample.edge_length &&
                   summary.layout == sample.layout && summary.operation == sample.operation;
        });
        if (found == result.end()) {
            result.push_back({sample.corpus, sample.edge_length, sample.layout, sample.operation, 0,
                              sample.work_items_per_iteration});
        }
    }
    for (auto& summary : result) {
        std::vector<double> values;
        for (const auto& sample : raw_samples) {
            if (summary.corpus == sample.corpus && summary.edge_length == sample.edge_length &&
                summary.layout == sample.layout && summary.operation == sample.operation) {
                values.push_back(sample.nanoseconds_per_iteration());
            }
        }
        std::ranges::sort(values);
        summary.sample_count = values.size();
        if (values.empty()) {
            continue;
        }
        summary.minimum_nanoseconds_per_iteration = values.front();
        summary.median_nanoseconds_per_iteration = percentile(values, 0.50);
        summary.p95_nanoseconds_per_iteration = percentile(values, 0.95);
        summary.maximum_nanoseconds_per_iteration = values.back();
        summary.mean_nanoseconds_per_iteration =
            std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
        double squared_difference_total = 0.0;
        for (const auto value : values) {
            const auto difference = value - summary.mean_nanoseconds_per_iteration;
            squared_difference_total += difference * difference;
        }
        summary.standard_deviation_nanoseconds =
            std::sqrt(squared_difference_total / static_cast<double>(values.size()));
        summary.coefficient_of_variation =
            summary.mean_nanoseconds_per_iteration == 0.0
                ? 0.0
                : summary.standard_deviation_nanoseconds / summary.mean_nanoseconds_per_iteration;
        summary.median_nanoseconds_per_work_item =
            summary.median_nanoseconds_per_iteration /
            static_cast<double>(summary.work_items_per_iteration);
    }
    return result;
}

std::string VoxelStorageBenchmarkReport::to_json() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n  \"schema_version\": " << schema_version
           << ",\n  \"benchmark\": \"voxel_storage\",\n  \"runtime\": {\n";
    const auto write_runtime_string = [&output](std::string_view name, std::string_view value,
                                                bool trailing) {
        output << "    \"" << name << "\": ";
        write_json_string(output, value);
        output << (trailing ? ",\n" : "\n");
    };
    write_runtime_string("engine_version", runtime.engine_version, true);
    write_runtime_string("git_commit", runtime.git_commit, true);
    write_runtime_string("build_configuration", runtime.build_configuration, true);
    write_runtime_string("compiler", runtime.compiler, true);
    write_runtime_string("platform", runtime.platform, true);
    write_runtime_string("architecture", runtime.architecture, true);
    write_runtime_string("operating_system", runtime.operating_system, true);
    write_runtime_string("cpu_model", runtime.cpu_model, true);
    output << "    \"logical_cpu_count\": " << runtime.logical_cpu_count
           << ",\n    \"git_dirty\": " << (runtime.git_dirty ? "true" : "false")
           << ",\n    \"tracy_enabled\": " << (runtime.tracy_enabled ? "true" : "false")
           << "\n  },\n  \"config\": {\n    \"seed\": " << config.seed
           << ",\n    \"material_count\": " << config.material_count
           << ",\n    \"warmup_repetitions\": " << config.warmup_repetitions
           << ",\n    \"repetitions\": " << config.repetitions
           << ",\n    \"iterations\": " << config.iterations
           << ",\n    \"random_edits_per_iteration\": " << config.random_edits_per_iteration
           << ",\n    \"palette_growth_edits\": " << config.palette_growth_edits
           << ",\n    \"corpora\": [";
    for (std::size_t index = 0; index < config.corpora.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        write_json_string(output, voxel_corpus_name(config.corpora[index]));
    }
    output << "],\n    \"edge_lengths\": [";
    for (std::size_t index = 0; index < config.edge_lengths.size(); ++index) {
        if (index != 0) {
            output << ", ";
        }
        output << config.edge_lengths[index];
    }
    output << "]\n  },\n  \"memory\": [\n";
    for (std::size_t measurement_index = 0; measurement_index < memory.size();
         ++measurement_index) {
        const auto& measurement = memory[measurement_index];
        output << "    {\n      \"corpus\": ";
        write_json_string(output, voxel_corpus_name(measurement.corpus));
        output << ",\n      \"edge_length\": " << measurement.edge_length
               << ",\n      \"cell_count\": " << measurement.corpus_stats.cell_count
               << ",\n      \"non_air_count\": " << measurement.corpus_stats.non_air_count
               << ",\n      \"unique_cell_count\": " << measurement.corpus_stats.unique_cell_count
               << ",\n      \"unique_block_value_count\": "
               << measurement.corpus_stats.unique_block_value_count
               << ",\n      \"metadata_cell_count\": "
               << measurement.corpus_stats.metadata_cell_count
               << ",\n      \"visible_face_count\": " << measurement.visible_face_count
               << ",\n      \"face_mask_payload_bytes\": " << measurement.face_mask_payload_bytes
               << ",\n      \"layouts\": [\n";
        for (std::size_t layout_index = 0; layout_index < measurement.layouts.size();
             ++layout_index) {
            const auto& layout = measurement.layouts[layout_index];
            const auto with_masks =
                layout.storage.allocated_bytes + measurement.face_mask_payload_bytes;
            output << "        {\n          \"layout\": ";
            write_json_string(output, voxel_storage_layout_name(layout.layout));
            output << ",\n          \"payload_bytes\": " << layout.storage.payload_bytes
                   << ",\n          \"allocated_bytes\": " << layout.storage.allocated_bytes
                   << ",\n          \"allocated_plus_face_masks_bytes\": " << with_masks
                   << ",\n          \"palette_size\": " << layout.storage.palette_size
                   << ",\n          \"packed_index_bytes\": " << layout.storage.packed_index_bytes
                   << ",\n          \"light_bytes\": " << layout.storage.light_bytes
                   << ",\n          \"metadata_entry_count\": "
                   << layout.storage.metadata_entry_count << ",\n          \"bits_per_index\": "
                   << static_cast<unsigned>(layout.storage.bits_per_index)
                   << ",\n          \"uniform_light\": "
                   << (layout.storage.uniform_light ? "true" : "false")
                   << ",\n          \"dense_blocks\": "
                   << (layout.storage.dense_blocks ? "true" : "false")
                   << ",\n          \"allocated_bytes_per_non_air\": ";
            write_ratio(output, layout.storage.allocated_bytes,
                        measurement.corpus_stats.non_air_count);
            output << ",\n          \"allocated_bytes_per_visible_face\": ";
            write_ratio(output, layout.storage.allocated_bytes, measurement.visible_face_count);
            output << ",\n          \"allocated_plus_face_masks_bytes_per_non_air\": ";
            write_ratio(output, with_masks, measurement.corpus_stats.non_air_count);
            output << ",\n          \"allocated_plus_face_masks_bytes_per_visible_face\": ";
            write_ratio(output, with_masks, measurement.visible_face_count);
            output << "\n        }"
                   << (layout_index + 1U == measurement.layouts.size() ? "\n" : ",\n");
        }
        output << "      ]\n    }" << (measurement_index + 1U == memory.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"raw_samples\": [\n";
    for (std::size_t index = 0; index < raw_samples.size(); ++index) {
        const auto& sample = raw_samples[index];
        output << "    {\"corpus\": ";
        write_json_string(output, voxel_corpus_name(sample.corpus));
        output << ", \"edge_length\": " << sample.edge_length << ", \"layout\": ";
        write_json_string(output, voxel_storage_layout_name(sample.layout));
        output << ", \"operation\": ";
        write_json_string(output, voxel_storage_operation_name(sample.operation));
        output << ", \"repetition\": " << sample.repetition
               << ", \"iteration_count\": " << sample.iteration_count
               << ", \"work_items_per_iteration\": " << sample.work_items_per_iteration
               << ", \"elapsed_nanoseconds\": " << sample.elapsed_nanoseconds
               << ", \"nanoseconds_per_iteration\": " << sample.nanoseconds_per_iteration()
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
        output << ", \"edge_length\": " << summary.edge_length << ", \"layout\": ";
        write_json_string(output, voxel_storage_layout_name(summary.layout));
        output << ", \"operation\": ";
        write_json_string(output, voxel_storage_operation_name(summary.operation));
        output << ", \"sample_count\": " << summary.sample_count
               << ", \"work_items_per_iteration\": " << summary.work_items_per_iteration
               << ", \"minimum_nanoseconds_per_iteration\": "
               << summary.minimum_nanoseconds_per_iteration
               << ", \"median_nanoseconds_per_iteration\": "
               << summary.median_nanoseconds_per_iteration
               << ", \"p95_nanoseconds_per_iteration\": " << summary.p95_nanoseconds_per_iteration
               << ", \"maximum_nanoseconds_per_iteration\": "
               << summary.maximum_nanoseconds_per_iteration
               << ", \"mean_nanoseconds_per_iteration\": " << summary.mean_nanoseconds_per_iteration
               << ", \"standard_deviation_nanoseconds\": " << summary.standard_deviation_nanoseconds
               << ", \"coefficient_of_variation\": " << summary.coefficient_of_variation
               << ", \"median_nanoseconds_per_work_item\": "
               << summary.median_nanoseconds_per_work_item << '}'
               << (index + 1U == summary_values.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

core::Status VoxelStorageBenchmarkReport::write_json(const std::filesystem::path& path) const {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return core::Status::failure("voxel_benchmark.create_directory_failed",
                                         "failed to create voxel benchmark output directory: " +
                                             error.message());
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return core::Status::failure("voxel_benchmark.open_output_failed",
                                     "failed to open voxel benchmark output: " + path.string());
    }
    const auto json = to_json();
    stream.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!stream) {
        return core::Status::failure("voxel_benchmark.write_output_failed",
                                     "failed to write voxel benchmark output: " + path.string());
    }
    return core::Status::ok();
}

core::Result<VoxelStorageBenchmarkReport>
run_voxel_storage_benchmark(const VoxelStorageBenchmarkConfig& config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<VoxelStorageBenchmarkReport>::failure(status.error().code,
                                                                  status.error().message);
    }
    VoxelStorageBenchmarkReport report;
    report.config = config;
    report.runtime = profiling::query_runtime_metadata();

    for (const auto corpus_kind : config.corpora) {
        for (const auto edge_length : config.edge_lengths) {
            auto generated = generate_voxel_storage_corpus(
                {corpus_kind, edge_length, config.material_count, config.seed});
            if (!generated) {
                return core::Result<VoxelStorageBenchmarkReport>::failure(
                    generated.error().code, generated.error().message);
            }
            auto corpus = std::move(generated).value();
            auto split = SplitVoxelSectionExperiment::encode(corpus.cells, edge_length);
            auto packed = PalettePackedVoxelSectionExperiment::encode(corpus.cells, edge_length);
            if (!split || !packed) {
                const auto& error = !split ? split.error() : packed.error();
                return core::Result<VoxelStorageBenchmarkReport>::failure(error.code,
                                                                          error.message);
            }
            const auto dense_masks = build_face_masks(edge_length, [&corpus](std::size_t index) {
                return !corpus.cells[index].is_air();
            });
            const auto split_masks = build_face_masks(edge_length, [&split](std::size_t index) {
                return split.value().block_type(index) != 0;
            });
            const auto packed_masks = build_face_masks(edge_length, [&packed](std::size_t index) {
                return packed.value().block_type(index) != 0;
            });
            if (dense_masks != split_masks || dense_masks != packed_masks) {
                return core::Result<VoxelStorageBenchmarkReport>::failure(
                    "voxel_benchmark.mask_mismatch",
                    "voxel storage layouts produced different occupancy or face masks");
            }
            VoxelCorpusMemoryMeasurement memory;
            memory.corpus = corpus_kind;
            memory.edge_length = edge_length;
            memory.corpus_stats = corpus.stats();
            memory.visible_face_count = dense_masks.visible_face_count;
            memory.face_mask_payload_bytes = dense_masks.payload_bytes();
            memory.layouts = {
                {VoxelStorageLayout::dense, dense_storage_stats(corpus)},
                {VoxelStorageLayout::split, split.value().stats()},
                {VoxelStorageLayout::palette_packed, packed.value().stats()},
            };
            report.memory.push_back(std::move(memory));

            const auto count = corpus.cells.size();
            const auto count_u32 = static_cast<std::uint32_t>(count);
            const auto indices = random_indices(count, config.seed ^ 0xa11ceULL);
            const auto edit_total =
                static_cast<std::size_t>(config.iterations) * config.random_edits_per_iteration;
            const auto edits = existing_palette_edits(corpus, edit_total, config.seed ^ 0xed17ULL);
            const auto growth = palette_growth_edits(config.palette_growth_edits, count,
                                                     config.seed ^ 0x6a09e667ULL);

            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::dense,
                VoxelStorageOperation::type_scan, count_u32,
                [&corpus](std::uint32_t) { return dense_type_checksum(corpus.cells); });
            collect_read_only(report, config, corpus_kind, edge_length, VoxelStorageLayout::split,
                              VoxelStorageOperation::type_scan, count_u32, [&split](std::uint32_t) {
                                  return split.value().scan_type_checksum();
                              });
            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::palette_packed,
                VoxelStorageOperation::type_scan, count_u32,
                [&packed](std::uint32_t) { return packed.value().scan_type_checksum(); });

            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::dense,
                VoxelStorageOperation::random_read, count_u32, [&corpus, &indices](std::uint32_t) {
                    return random_read_checksum(
                        indices, [&corpus](std::size_t index) { return corpus.cells[index]; });
                });
            collect_read_only(report, config, corpus_kind, edge_length, VoxelStorageLayout::split,
                              VoxelStorageOperation::random_read, count_u32,
                              [&split, &indices](std::uint32_t) {
                                  return random_read_checksum(indices, [&split](std::size_t index) {
                                      return split.value().cell(index);
                                  });
                              });
            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::palette_packed,
                VoxelStorageOperation::random_read, count_u32, [&packed, &indices](std::uint32_t) {
                    return random_read_checksum(indices, [&packed](std::size_t index) {
                        return packed.value().cell(index);
                    });
                });

            const auto edit_execute = [&edits, &config](auto& state, std::uint32_t iteration) {
                std::uint64_t checksum = 0;
                const auto begin =
                    static_cast<std::size_t>(iteration) * config.random_edits_per_iteration;
                for (std::uint32_t edit = 0; edit < config.random_edits_per_iteration; ++edit) {
                    const auto& command = edits[begin + edit];
                    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(state)>,
                                                 std::vector<VoxelCell>>) {
                        state[command.index] = command.value;
                    } else {
                        if (!state.set(command.index, command.value)) {
                            std::terminate();
                        }
                    }
                    checksum = cell_checksum(checksum, command.value);
                }
                return checksum;
            };
            collect_stateful(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::dense,
                VoxelStorageOperation::random_edit_existing, config.iterations,
                config.random_edits_per_iteration, [&corpus] { return corpus.cells; }, edit_execute,
                [count](const auto& state) {
                    return full_cell_checksum(count,
                                              [&state](std::size_t index) { return state[index]; });
                });
            collect_stateful(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::split,
                VoxelStorageOperation::random_edit_existing, config.iterations,
                config.random_edits_per_iteration, [&split] { return split.value(); }, edit_execute,
                [count](const auto& state) {
                    return full_cell_checksum(
                        count, [&state](std::size_t index) { return state.cell(index); });
                });
            collect_stateful(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::palette_packed,
                VoxelStorageOperation::random_edit_existing, config.iterations,
                config.random_edits_per_iteration, [&packed] { return packed.value(); },
                edit_execute,
                [count](const auto& state) {
                    return full_cell_checksum(
                        count, [&state](std::size_t index) { return state.cell(index); });
                });

            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::dense,
                VoxelStorageOperation::encode, count_u32, [&corpus](std::uint32_t iteration) {
                    auto encoded = corpus.cells;
                    observe_memory<VoxelCell>(encoded);
                    return cell_checksum(encoded.size(), encoded[iteration % encoded.size()]);
                });
            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::split,
                VoxelStorageOperation::encode, count_u32,
                [&corpus, edge_length](std::uint32_t iteration) {
                    auto encoded = SplitVoxelSectionExperiment::encode(corpus.cells, edge_length);
                    if (!encoded) {
                        std::terminate();
                    }
                    return cell_checksum(encoded.value().stats().payload_bytes,
                                         encoded.value().cell(iteration % corpus.cells.size()));
                });
            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::palette_packed,
                VoxelStorageOperation::encode, count_u32,
                [&corpus, edge_length](std::uint32_t iteration) {
                    auto encoded =
                        PalettePackedVoxelSectionExperiment::encode(corpus.cells, edge_length);
                    if (!encoded) {
                        std::terminate();
                    }
                    return cell_checksum(encoded.value().stats().payload_bytes,
                                         encoded.value().cell(iteration % corpus.cells.size()));
                });

            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::dense,
                VoxelStorageOperation::decode, count_u32, [&corpus](std::uint32_t iteration) {
                    auto decoded = corpus.cells;
                    observe_memory<VoxelCell>(decoded);
                    return cell_checksum(decoded.size(), decoded[iteration % decoded.size()]);
                });
            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::split,
                VoxelStorageOperation::decode, count_u32, [&split](std::uint32_t iteration) {
                    const auto decoded = split.value().decode();
                    observe_memory<VoxelCell>(decoded);
                    return cell_checksum(decoded.size(), decoded[iteration % decoded.size()]);
                });
            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::palette_packed,
                VoxelStorageOperation::decode, count_u32, [&packed](std::uint32_t iteration) {
                    const auto decoded = packed.value().decode();
                    observe_memory<VoxelCell>(decoded);
                    return cell_checksum(decoded.size(), decoded[iteration % decoded.size()]);
                });

            collect_read_only(report, config, corpus_kind, edge_length, VoxelStorageLayout::dense,
                              VoxelStorageOperation::serialize, count_u32,
                              [&corpus](std::uint32_t) {
                                  const auto bytes = serialize_canonical(
                                      corpus.cells.size(),
                                      [&corpus](std::size_t index) { return corpus.cells[index]; });
                                  return byte_checksum(bytes);
                              });
            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::split,
                VoxelStorageOperation::serialize, count_u32, [&split, count](std::uint32_t) {
                    const auto bytes = serialize_canonical(
                        count, [&split](std::size_t index) { return split.value().cell(index); });
                    return byte_checksum(bytes);
                });
            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::palette_packed,
                VoxelStorageOperation::serialize, count_u32, [&packed, count](std::uint32_t) {
                    const auto bytes = serialize_canonical(
                        count, [&packed](std::size_t index) { return packed.value().cell(index); });
                    return byte_checksum(bytes);
                });

            collect_read_only(report, config, corpus_kind, edge_length, VoxelStorageLayout::dense,
                              VoxelStorageOperation::face_mask_build, count_u32,
                              [&corpus, edge_length](std::uint32_t) {
                                  return build_face_masks(edge_length,
                                                          [&corpus](std::size_t index) {
                                                              return !corpus.cells[index].is_air();
                                                          })
                                      .checksum();
                              });
            collect_read_only(report, config, corpus_kind, edge_length, VoxelStorageLayout::split,
                              VoxelStorageOperation::face_mask_build, count_u32,
                              [&split, edge_length](std::uint32_t) {
                                  return build_face_masks(edge_length,
                                                          [&split](std::size_t index) {
                                                              return split.value().block_type(
                                                                         index) != 0;
                                                          })
                                      .checksum();
                              });
            collect_read_only(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::palette_packed,
                VoxelStorageOperation::face_mask_build, count_u32,
                [&packed, edge_length](std::uint32_t) {
                    return build_face_masks(edge_length,
                                            [&packed](std::size_t index) {
                                                return packed.value().block_type(index) != 0;
                                            })
                        .checksum();
                });

            collect_stateful(
                report, config, corpus_kind, edge_length, VoxelStorageLayout::palette_packed,
                VoxelStorageOperation::palette_growth_edit, 1, config.palette_growth_edits,
                [&packed] { return packed.value(); },
                [&growth](auto& state, std::uint32_t) {
                    std::uint64_t checksum = 0;
                    for (const auto& command : growth) {
                        if (!state.set(command.index, command.value)) {
                            std::terminate();
                        }
                        checksum = cell_checksum(checksum, command.value);
                    }
                    return checksum;
                },
                [count](const auto& state) {
                    return full_cell_checksum(
                        count, [&state](std::size_t index) { return state.cell(index); });
                });
        }
    }
    status = report.validate();
    if (!status) {
        return core::Result<VoxelStorageBenchmarkReport>::failure(status.error().code,
                                                                  status.error().message);
    }
    return core::Result<VoxelStorageBenchmarkReport>::success(std::move(report));
}

} // namespace heartstead::world::benchmark
