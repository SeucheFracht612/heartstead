#include "engine/world/streaming/chunk_streaming_benchmark.hpp"

#include "engine/profiling/profiler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <utility>

namespace heartstead::world::benchmark {

namespace {

using BenchmarkClock = std::chrono::steady_clock;

constexpr ChunkCoord near_center{4'096, 0, -4'096};
constexpr ChunkCoord teleport_old_center{-8'192, 0, 8'192};
constexpr ChunkCoord teleport_target_center{8'192, 0, -8'192};

struct WorkloadExecution {
    ChunkStreamingBenchmarkRun run;
    std::vector<ChunkStreamingBenchmarkSample> samples;
};

[[nodiscard]] bool valid_workload(ChunkStreamingWorkload workload) noexcept {
    return workload == ChunkStreamingWorkload::near_load ||
           workload == ChunkStreamingWorkload::teleport_recovery;
}

[[nodiscard]] std::uint64_t elapsed_microseconds(BenchmarkClock::time_point begin,
                                                 BenchmarkClock::time_point end) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

[[nodiscard]] std::vector<ChunkCoord> circular_interest(ChunkCoord center, std::uint16_t radius) {
    std::vector<ChunkCoord> result;
    const auto signed_radius = static_cast<std::int64_t>(radius);
    const auto radius_squared = signed_radius * signed_radius;
    for (std::int64_t z = -signed_radius; z <= signed_radius; ++z) {
        for (std::int64_t x = -signed_radius; x <= signed_radius; ++x) {
            if (x * x + z * z <= radius_squared) {
                result.push_back({center.x + x, center.y, center.z + z});
            }
        }
    }
    std::ranges::sort(result, [center](ChunkCoord lhs, ChunkCoord rhs) {
        const auto lhs_x = lhs.x - center.x;
        const auto lhs_z = lhs.z - center.z;
        const auto rhs_x = rhs.x - center.x;
        const auto rhs_z = rhs.z - center.z;
        const auto lhs_distance = lhs_x * lhs_x + lhs_z * lhs_z;
        const auto rhs_distance = rhs_x * rhs_x + rhs_z * rhs_z;
        return lhs_distance != rhs_distance ? lhs_distance < rhs_distance : lhs < rhs;
    });
    return result;
}

[[nodiscard]] core::Result<ChunkLoadSchedulerContext> make_benchmark_context(std::uint64_t seed) {
    const auto stone_id = core::PrototypeId::parse("benchmark:voxels/stone");
    if (!stone_id) {
        return core::Result<ChunkLoadSchedulerContext>::failure(
            "chunk_streaming_benchmark.invalid_voxel_id",
            "the built-in benchmark voxel id is invalid");
    }

    VoxelDefinition stone;
    stone.type = 1;
    stone.prototype_id = *stone_id;
    stone.display_name = "Benchmark stone";
    stone.terrain_material = "benchmark_stone";
    stone.mining_tool = "pickaxe";

    RegionDescriptor region;
    region.id = "benchmark_region";
    region.age = "benchmark_age";
    region.biome_cluster = "benchmark_biome";
    region.resource_rules.push_back({*stone_id, "terrain", 1.0});

    ChunkLoadSchedulerContext context;
    context.generation.world_seed = seed;
    context.generation.region_id = region.id;
    context.generation.base_surface_y = 12;
    context.generation.surface_variation = 10;
    context.generation.enable_caves = true;
    context.generation.cave_frequency_per_mille = 70;
    context.generation.cave_min_depth = 6;
    auto status = context.regions.add_region(std::move(region));
    if (!status) {
        return core::Result<ChunkLoadSchedulerContext>::failure(status.error().code,
                                                                status.error().message);
    }
    status = context.palette.add(std::move(stone));
    if (!status) {
        return core::Result<ChunkLoadSchedulerContext>::failure(status.error().code,
                                                                status.error().message);
    }
    return core::Result<ChunkLoadSchedulerContext>::success(std::move(context));
}

[[nodiscard]] core::Status submit_available(ChunkLoadScheduler& scheduler,
                                            std::span<const ChunkCoord> desired,
                                            std::size_t& next_submission,
                                            ChunkStreamingBenchmarkRun& run) {
    while (next_submission < desired.size() && scheduler.has_capacity()) {
        auto submitted = scheduler.submit(desired[next_submission], jobs::JobPriority::high);
        if (!submitted) {
            return core::Status::failure(submitted.error().code, submitted.error().message);
        }
        ++next_submission;
    }
    if (next_submission < desired.size() && !scheduler.has_capacity()) {
        ++run.admission_deferred_updates;
    }
    return core::Status::ok();
}

[[nodiscard]] const ChunkLoadTimingSample*
find_success_timing(const ChunkLoadPublicationReport& report, ChunkCoord coord) noexcept {
    const auto found = std::ranges::find_if(report.timings, [coord](const auto& timing) {
        return timing.coord == coord && timing.state == ChunkLoadResultState::succeeded;
    });
    return found == report.timings.end() ? nullptr : &*found;
}

[[nodiscard]] core::Result<WorkloadExecution>
run_workload(const ChunkStreamingBenchmarkConfig& config, ChunkStreamingWorkload workload,
             std::uint32_t repetition) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.chunk_streaming.workload");
    auto context = make_benchmark_context(config.seed);
    if (!context) {
        return core::Result<WorkloadExecution>::failure(context.error().code,
                                                        context.error().message);
    }
    auto created = ChunkLoadScheduler::create(std::move(context).value(), config.scheduler);
    if (!created) {
        return core::Result<WorkloadExecution>::failure(created.error().code,
                                                        created.error().message);
    }
    auto scheduler = std::move(created).value();
    WorldState state;

    const auto target_center =
        workload == ChunkStreamingWorkload::near_load ? near_center : teleport_target_center;
    const auto target = circular_interest(target_center, config.radius_chunks);
    const std::set<ChunkCoord> target_set(target.begin(), target.end());

    WorkloadExecution execution;
    execution.run.workload = workload;
    execution.run.repetition = repetition;
    execution.run.desired_chunks = target.size();
    execution.samples.reserve(target.size());

    if (workload == ChunkStreamingWorkload::teleport_recovery) {
        const auto obsolete = circular_interest(teleport_old_center, config.radius_chunks);
        std::size_t next_obsolete = 0;
        while (next_obsolete < obsolete.size() && scheduler->has_capacity()) {
            auto submitted = scheduler->submit(obsolete[next_obsolete], jobs::JobPriority::normal);
            if (!submitted) {
                return core::Result<WorkloadExecution>::failure(submitted.error().code,
                                                                submitted.error().message);
            }
            ++next_obsolete;
        }
        execution.run.obsolete_requests = next_obsolete;
        if (next_obsolete == 0 || scheduler->cancel_all_except(target) != next_obsolete) {
            return core::Result<WorkloadExecution>::failure(
                "chunk_streaming_benchmark.teleport_cancellation_failed",
                "teleport workload did not cancel every primed obsolete request");
        }
    }

    // All target coordinates become interesting at one wall-clock instant. Admission retries use
    // this original timestamp, so queue saturation remains visible instead of creating coordinated
    // omission by starting each latency sample only after a slot opens.
    const auto interest_started_at = BenchmarkClock::now();
    const auto deadline = interest_started_at + std::chrono::milliseconds(config.timeout_ms);
    std::size_t next_submission = 0;
    if (workload == ChunkStreamingWorkload::near_load) {
        auto status = submit_available(*scheduler, target, next_submission, execution.run);
        if (!status) {
            return core::Result<WorkloadExecution>::failure(status.error().code,
                                                            status.error().message);
        }
        // Initial saturation happens at interest declaration, not after a publication update.
        execution.run.admission_deferred_updates = 0;
    }

    std::set<ChunkCoord> published;
    while (published.size() < target.size() || scheduler->has_in_flight() ||
           next_submission < target.size()) {
        if (BenchmarkClock::now() >= deadline) {
            return core::Result<WorkloadExecution>::failure(
                "chunk_streaming_benchmark.timeout",
                "chunk streaming workload exceeded its declared wall-clock timeout");
        }
        std::this_thread::sleep_for(std::chrono::microseconds(config.update_interval_us));

        auto publication = scheduler->update(state);
        const auto publication_finished_at = BenchmarkClock::now();
        execution.run.item_budget_exhaustions += publication.item_budget_exhausted ? 1U : 0U;
        execution.run.time_budget_exhaustions += publication.time_budget_exhausted ? 1U : 0U;
        execution.run.maximum_publication_time_us =
            std::max(execution.run.maximum_publication_time_us, publication.publication_time_us);

        if (!publication.failures.empty()) {
            const auto& failure = publication.failures.front();
            return core::Result<WorkloadExecution>::failure(failure.error.code,
                                                            failure.error.message);
        }
        if (!publication.stale.empty()) {
            return core::Result<WorkloadExecution>::failure(
                "chunk_streaming_benchmark.stale_result",
                "benchmark workload produced an unexpected stale chunk load");
        }

        for (const auto& load : publication.published) {
            if (!target_set.contains(load.coord)) {
                ++execution.run.off_interest_publications;
                continue;
            }
            if (!published.insert(load.coord).second) {
                return core::Result<WorkloadExecution>::failure(
                    "chunk_streaming_benchmark.duplicate_publication",
                    "benchmark workload published one target chunk more than once");
            }
            const auto* timing = find_success_timing(publication, load.coord);
            if (timing == nullptr) {
                return core::Result<WorkloadExecution>::failure(
                    "chunk_streaming_benchmark.missing_timing",
                    "published chunk has no successful lifecycle timing sample");
            }
            const auto ordinal =
                static_cast<std::uint32_t>(std::ranges::find(target, load.coord) - target.begin());
            execution.samples.push_back(
                {workload, repetition, ordinal, load.coord, timing->request_id.value(),
                 elapsed_microseconds(interest_started_at, publication_finished_at),
                 timing->pipeline_latency_ms, timing->disk_read_ms, timing->decode_ms,
                 timing->generation_ms, timing->prepare_ms, timing->worker_ms});
        }
        if (execution.run.off_interest_publications != 0) {
            return core::Result<WorkloadExecution>::failure(
                "chunk_streaming_benchmark.off_interest_publication",
                "cancelled obsolete work published after the teleport interest change");
        }

        auto status = submit_available(*scheduler, target, next_submission, execution.run);
        if (!status) {
            return core::Result<WorkloadExecution>::failure(status.error().code,
                                                            status.error().message);
        }
    }

    const auto& stats = scheduler->stats();
    execution.run.submitted_requests = stats.submitted_requests;
    execution.run.published_requests = stats.published_requests;
    execution.run.cancelled_requests = stats.cancelled_requests;
    execution.run.stale_requests = stats.stale_requests;
    execution.run.failed_requests = stats.failed_requests;
    execution.run.rejected_requests = stats.rejected_requests;
    execution.run.elapsed_us = elapsed_microseconds(interest_started_at, BenchmarkClock::now());
    execution.run.maximum_publication_time_us =
        std::max(execution.run.maximum_publication_time_us, stats.maximum_publication_time_us);
    execution.run.reserved_working_bytes_high_water = stats.reserved_working_bytes_high_water;
    execution.run.final_reserved_working_bytes = stats.reserved_working_bytes;

    if (execution.samples.size() != target.size() ||
        state.chunks().chunk_count() != target.size() ||
        execution.run.published_requests != target.size() ||
        execution.run.final_reserved_working_bytes != 0) {
        return core::Result<WorkloadExecution>::failure(
            "chunk_streaming_benchmark.incomplete",
            "chunk streaming workload did not converge and release all reservations");
    }
    if (workload == ChunkStreamingWorkload::teleport_recovery &&
        execution.run.cancelled_requests != execution.run.obsolete_requests) {
        return core::Result<WorkloadExecution>::failure(
            "chunk_streaming_benchmark.cancellation_mismatch",
            "teleport workload did not retire every obsolete request as cancelled");
    }
    return core::Result<WorkloadExecution>::success(std::move(execution));
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
    std::string escaped;
    escaped.reserve(value.size());
    for (const auto character : value) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"' << json_escape(value) << '"';
}

void write_runtime_metadata(std::ostream& output, const profiling::RuntimeMetadata& runtime) {
    output << "  \"runtime\": {\n";
    const auto string_field = [&output](std::string_view name, std::string_view value,
                                        bool trailing_comma = true) {
        output << "    \"" << name << "\": ";
        write_json_string(output, value);
        output << (trailing_comma ? ",\n" : "\n");
    };
    string_field("engine_version", runtime.engine_version);
    string_field("git_commit", runtime.git_commit);
    string_field("build_configuration", runtime.build_configuration);
    string_field("compiler", runtime.compiler);
    string_field("platform", runtime.platform);
    string_field("architecture", runtime.architecture);
    string_field("operating_system", runtime.operating_system);
    string_field("cpu_model", runtime.cpu_model);
    output << "    \"logical_cpu_count\": " << runtime.logical_cpu_count << ",\n"
           << "    \"git_dirty\": " << (runtime.git_dirty ? "true" : "false") << ",\n"
           << "    \"tracy_enabled\": " << (runtime.tracy_enabled ? "true" : "false") << "\n  },\n";
}

[[nodiscard]] core::Status write_text_file(const std::filesystem::path& path,
                                           std::string_view text) {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return core::Status::failure("chunk_streaming_benchmark.create_directory_failed",
                                         "failed to create benchmark output directory: " +
                                             error.message());
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return core::Status::failure("chunk_streaming_benchmark.open_output_failed",
                                     "failed to open benchmark output: " + path.string());
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        return core::Status::failure("chunk_streaming_benchmark.write_output_failed",
                                     "failed to write benchmark output: " + path.string());
    }
    return core::Status::ok();
}

} // namespace

std::string_view chunk_streaming_workload_name(ChunkStreamingWorkload workload) noexcept {
    switch (workload) {
    case ChunkStreamingWorkload::near_load:
        return "near_load";
    case ChunkStreamingWorkload::teleport_recovery:
        return "teleport_recovery";
    }
    return "unknown";
}

ChunkStreamingBenchmarkConfig::ChunkStreamingBenchmarkConfig()
    : workloads{ChunkStreamingWorkload::near_load, ChunkStreamingWorkload::teleport_recovery} {}

core::Status ChunkStreamingBenchmarkConfig::validate() const {
    if (workloads.empty()) {
        return core::Status::failure("chunk_streaming_benchmark.empty_workloads",
                                     "at least one streaming workload is required");
    }
    std::set<ChunkStreamingWorkload> unique;
    for (const auto workload : workloads) {
        if (!valid_workload(workload) || !unique.insert(workload).second) {
            return core::Status::failure(
                "chunk_streaming_benchmark.invalid_workload",
                "streaming workloads must be known and cannot be duplicated");
        }
    }
    if (radius_chunks == 0 || radius_chunks > 16) {
        return core::Status::failure(
            "chunk_streaming_benchmark.invalid_radius",
            "streaming benchmark radius must be between one and sixteen chunks");
    }
    if (repetitions == 0 || repetitions > 100 || warmup_repetitions > 100) {
        return core::Status::failure(
            "chunk_streaming_benchmark.invalid_repetitions",
            "streaming benchmark repetitions must be 1..100 and warmups 0..100");
    }
    if (update_interval_us == 0 || update_interval_us > 1'000'000 || timeout_ms == 0 ||
        timeout_ms > 600'000) {
        return core::Status::failure(
            "chunk_streaming_benchmark.invalid_timing",
            "update cadence and workload timeout must be positive and bounded");
    }
    if (!std::isfinite(maximum_near_p95_ms) || maximum_near_p95_ms <= 0.0 ||
        !std::isfinite(maximum_teleport_p95_ms) || maximum_teleport_p95_ms <= 0.0 ||
        maximum_owner_publication_us == 0) {
        return core::Status::failure("chunk_streaming_benchmark.invalid_gates",
                                     "streaming latency and publication gates must be positive");
    }
    return scheduler.validate();
}

core::Status ChunkStreamingBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    const auto desired_count = circular_interest(near_center, config.radius_chunks).size();
    const auto expected_run_count = config.workloads.size() * config.repetitions;
    if (runs.size() != expected_run_count ||
        raw_samples.size() != expected_run_count * desired_count) {
        return core::Status::failure(
            "chunk_streaming_benchmark.incomplete_report",
            "streaming report does not contain every configured run and raw chunk sample");
    }

    std::set<std::pair<ChunkStreamingWorkload, std::uint32_t>> run_keys;
    for (const auto& run : runs) {
        if (!valid_workload(run.workload) || run.repetition >= config.repetitions ||
            std::ranges::find(config.workloads, run.workload) == config.workloads.end() ||
            !run_keys.insert({run.workload, run.repetition}).second) {
            return core::Status::failure("chunk_streaming_benchmark.invalid_run_key",
                                         "streaming report contains an invalid or duplicate run");
        }
        if (run.desired_chunks != desired_count || run.published_requests != desired_count ||
            run.stale_requests != 0 || run.failed_requests != 0 || run.rejected_requests != 0 ||
            run.off_interest_publications != 0 || run.final_reserved_working_bytes != 0 ||
            run.reserved_working_bytes_high_water > config.scheduler.max_reserved_working_bytes) {
            return core::Status::failure(
                "chunk_streaming_benchmark.invalid_run",
                "streaming benchmark run did not converge within its hard bounds");
        }
        if ((run.workload == ChunkStreamingWorkload::near_load && run.obsolete_requests != 0) ||
            (run.workload == ChunkStreamingWorkload::teleport_recovery &&
             (run.obsolete_requests == 0 || run.cancelled_requests != run.obsolete_requests))) {
            return core::Status::failure(
                "chunk_streaming_benchmark.invalid_cancellation",
                "streaming report has inconsistent teleport cancellation counts");
        }
    }

    std::set<std::tuple<ChunkStreamingWorkload, std::uint32_t, ChunkCoord>> sample_keys;
    for (const auto& sample : raw_samples) {
        if (!valid_workload(sample.workload) || sample.repetition >= config.repetitions ||
            !run_keys.contains({sample.workload, sample.repetition}) ||
            sample.ordinal >= desired_count || sample.request_id == 0 ||
            !std::isfinite(sample.scheduler_pipeline_ms) || sample.scheduler_pipeline_ms < 0.0 ||
            !std::isfinite(sample.disk_read_ms) || sample.disk_read_ms < 0.0 ||
            !std::isfinite(sample.decode_ms) || sample.decode_ms < 0.0 ||
            !std::isfinite(sample.generation_ms) || sample.generation_ms < 0.0 ||
            !std::isfinite(sample.prepare_ms) || sample.prepare_ms < 0.0 ||
            !std::isfinite(sample.worker_ms) || sample.worker_ms < 0.0 ||
            !sample_keys.insert({sample.workload, sample.repetition, sample.coord}).second) {
            return core::Status::failure(
                "chunk_streaming_benchmark.invalid_sample",
                "streaming report contains an invalid or duplicate raw sample");
        }
        const auto center = sample.workload == ChunkStreamingWorkload::near_load
                                ? near_center
                                : teleport_target_center;
        const auto desired = circular_interest(center, config.radius_chunks);
        if (std::ranges::find(desired, sample.coord) == desired.end()) {
            return core::Status::failure(
                "chunk_streaming_benchmark.sample_outside_interest",
                "streaming raw sample is outside its declared target interest set");
        }
    }
    return core::Status::ok();
}

std::vector<ChunkStreamingBenchmarkSummary> ChunkStreamingBenchmarkReport::summaries() const {
    std::vector<ChunkStreamingBenchmarkSummary> result;
    result.reserve(config.workloads.size());
    for (const auto workload : config.workloads) {
        ChunkStreamingBenchmarkSummary summary;
        summary.workload = workload;
        std::vector<double> interest_ms;
        std::vector<double> pipeline_ms;
        std::vector<double> generation_ms;
        std::vector<double> worker_ms;
        for (const auto& sample : raw_samples) {
            if (sample.workload != workload) {
                continue;
            }
            interest_ms.push_back(static_cast<double>(sample.interest_to_publication_us) / 1'000.0);
            pipeline_ms.push_back(sample.scheduler_pipeline_ms);
            generation_ms.push_back(sample.generation_ms);
            worker_ms.push_back(sample.worker_ms);
        }
        std::ranges::sort(interest_ms);
        std::ranges::sort(pipeline_ms);
        std::ranges::sort(generation_ms);
        std::ranges::sort(worker_ms);
        summary.sample_count = interest_ms.size();
        summary.median_interest_to_publication_ms = percentile(interest_ms, 0.50);
        summary.p95_interest_to_publication_ms = percentile(interest_ms, 0.95);
        summary.p99_interest_to_publication_ms = percentile(interest_ms, 0.99);
        summary.maximum_interest_to_publication_ms = interest_ms.empty() ? 0.0 : interest_ms.back();
        summary.median_scheduler_pipeline_ms = percentile(pipeline_ms, 0.50);
        summary.p95_scheduler_pipeline_ms = percentile(pipeline_ms, 0.95);
        summary.p95_generation_ms = percentile(generation_ms, 0.95);
        summary.p95_worker_ms = percentile(worker_ms, 0.95);

        double throughput_total = 0.0;
        for (const auto& run : runs) {
            if (run.workload != workload) {
                continue;
            }
            ++summary.run_count;
            summary.total_cancelled_requests += run.cancelled_requests;
            summary.total_admission_deferred_updates += run.admission_deferred_updates;
            summary.maximum_publication_time_us =
                std::max(summary.maximum_publication_time_us, run.maximum_publication_time_us);
            summary.reserved_working_bytes_high_water = std::max(
                summary.reserved_working_bytes_high_water, run.reserved_working_bytes_high_water);
            if (run.elapsed_us != 0) {
                throughput_total += static_cast<double>(run.desired_chunks) * 1'000'000.0 /
                                    static_cast<double>(run.elapsed_us);
            }
        }
        summary.mean_chunks_per_second =
            summary.run_count == 0 ? 0.0
                                   : throughput_total / static_cast<double>(summary.run_count);

        summary.gates.evaluated = config.enforce_gates;
        if (config.enforce_gates) {
            const auto latency_limit = workload == ChunkStreamingWorkload::near_load
                                           ? config.maximum_near_p95_ms
                                           : config.maximum_teleport_p95_ms;
            const auto check = [&summary](std::string metric, double actual, double limit) {
                if (!std::isfinite(actual) || actual > limit) {
                    summary.gates.violations.push_back({std::move(metric), actual, limit});
                }
            };
            check("p95_interest_to_publication_ms", summary.p95_interest_to_publication_ms,
                  latency_limit);
            check("maximum_publication_time_us",
                  static_cast<double>(summary.maximum_publication_time_us),
                  static_cast<double>(config.maximum_owner_publication_us));
            summary.gates.passed = summary.gates.violations.empty();
        }
        result.push_back(std::move(summary));
    }
    return result;
}

bool ChunkStreamingBenchmarkReport::gates_passed() const {
    const auto values = summaries();
    return std::ranges::all_of(values, [](const auto& summary) { return summary.gates.passed; });
}

std::string ChunkStreamingBenchmarkReport::to_json() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n"
           << "  \"schema_version\": " << schema_version << ",\n"
           << "  \"benchmark\": \"chunk_streaming\",\n"
           << "  \"config\": {\n"
           << "    \"seed\": " << config.seed << ",\n"
           << "    \"radius_chunks\": " << config.radius_chunks << ",\n"
           << "    \"warmup_repetitions\": " << config.warmup_repetitions << ",\n"
           << "    \"repetitions\": " << config.repetitions << ",\n"
           << "    \"update_interval_us\": " << config.update_interval_us << ",\n"
           << "    \"timeout_ms\": " << config.timeout_ms << ",\n"
           << "    \"enforce_gates\": " << (config.enforce_gates ? "true" : "false") << ",\n"
           << "    \"maximum_near_p95_ms\": " << config.maximum_near_p95_ms << ",\n"
           << "    \"maximum_teleport_p95_ms\": " << config.maximum_teleport_p95_ms << ",\n"
           << "    \"maximum_owner_publication_us\": " << config.maximum_owner_publication_us
           << ",\n"
           << "    \"workloads\": [";
    for (std::size_t index = 0; index < config.workloads.size(); ++index) {
        output << (index == 0 ? "" : ", ");
        write_json_string(output, chunk_streaming_workload_name(config.workloads[index]));
    }
    output << "],\n"
           << "    \"scheduler\": {\n"
           << "      \"worker_count\": " << config.scheduler.worker_count << ",\n"
           << "      \"max_concurrent_requests\": " << config.scheduler.max_concurrent_requests
           << ",\n"
           << "      \"max_completed_results\": " << config.scheduler.max_completed_results << ",\n"
           << "      \"reservation_bytes_per_request\": "
           << config.scheduler.reservation_bytes_per_request << ",\n"
           << "      \"max_reserved_working_bytes\": "
           << config.scheduler.max_reserved_working_bytes << ",\n"
           << "      \"max_publications_per_update\": "
           << config.scheduler.max_publications_per_update << ",\n"
           << "      \"max_publication_time_us\": " << config.scheduler.max_publication_time_us
           << "\n"
           << "    }\n"
           << "  },\n";
    write_runtime_metadata(output, runtime);

    output << "  \"runs\": [\n";
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const auto& run = runs[index];
        output << "    {\"workload\": ";
        write_json_string(output, chunk_streaming_workload_name(run.workload));
        output << ", \"repetition\": " << run.repetition
               << ", \"desired_chunks\": " << run.desired_chunks
               << ", \"obsolete_requests\": " << run.obsolete_requests
               << ", \"submitted_requests\": " << run.submitted_requests
               << ", \"published_requests\": " << run.published_requests
               << ", \"cancelled_requests\": " << run.cancelled_requests
               << ", \"stale_requests\": " << run.stale_requests
               << ", \"failed_requests\": " << run.failed_requests
               << ", \"rejected_requests\": " << run.rejected_requests
               << ", \"off_interest_publications\": " << run.off_interest_publications
               << ", \"admission_deferred_updates\": " << run.admission_deferred_updates
               << ", \"item_budget_exhaustions\": " << run.item_budget_exhaustions
               << ", \"time_budget_exhaustions\": " << run.time_budget_exhaustions
               << ", \"elapsed_us\": " << run.elapsed_us
               << ", \"maximum_publication_time_us\": " << run.maximum_publication_time_us
               << ", \"reserved_working_bytes_high_water\": "
               << run.reserved_working_bytes_high_water
               << ", \"final_reserved_working_bytes\": " << run.final_reserved_working_bytes << '}'
               << (index + 1U == runs.size() ? "\n" : ",\n");
    }
    output << "  ],\n";

    output << "  \"raw_samples\": [\n";
    for (std::size_t index = 0; index < raw_samples.size(); ++index) {
        const auto& sample = raw_samples[index];
        output << "    {\"workload\": ";
        write_json_string(output, chunk_streaming_workload_name(sample.workload));
        output << ", \"repetition\": " << sample.repetition << ", \"ordinal\": " << sample.ordinal
               << ", \"coord\": [" << sample.coord.x << ", " << sample.coord.y << ", "
               << sample.coord.z << ']' << ", \"request_id\": " << sample.request_id
               << ", \"interest_to_publication_us\": " << sample.interest_to_publication_us
               << ", \"scheduler_pipeline_ms\": " << sample.scheduler_pipeline_ms
               << ", \"disk_read_ms\": " << sample.disk_read_ms
               << ", \"decode_ms\": " << sample.decode_ms
               << ", \"generation_ms\": " << sample.generation_ms
               << ", \"prepare_ms\": " << sample.prepare_ms
               << ", \"worker_ms\": " << sample.worker_ms << '}'
               << (index + 1U == raw_samples.size() ? "\n" : ",\n");
    }
    output << "  ],\n";

    const auto summary_values = summaries();
    output << "  \"summaries\": [\n";
    for (std::size_t index = 0; index < summary_values.size(); ++index) {
        const auto& summary = summary_values[index];
        output << "    {\"workload\": ";
        write_json_string(output, chunk_streaming_workload_name(summary.workload));
        output << ", \"run_count\": " << summary.run_count
               << ", \"sample_count\": " << summary.sample_count
               << ", \"median_interest_to_publication_ms\": "
               << summary.median_interest_to_publication_ms
               << ", \"p95_interest_to_publication_ms\": " << summary.p95_interest_to_publication_ms
               << ", \"p99_interest_to_publication_ms\": " << summary.p99_interest_to_publication_ms
               << ", \"maximum_interest_to_publication_ms\": "
               << summary.maximum_interest_to_publication_ms
               << ", \"median_scheduler_pipeline_ms\": " << summary.median_scheduler_pipeline_ms
               << ", \"p95_scheduler_pipeline_ms\": " << summary.p95_scheduler_pipeline_ms
               << ", \"p95_generation_ms\": " << summary.p95_generation_ms
               << ", \"p95_worker_ms\": " << summary.p95_worker_ms
               << ", \"mean_chunks_per_second\": " << summary.mean_chunks_per_second
               << ", \"total_cancelled_requests\": " << summary.total_cancelled_requests
               << ", \"total_admission_deferred_updates\": "
               << summary.total_admission_deferred_updates
               << ", \"maximum_publication_time_us\": " << summary.maximum_publication_time_us
               << ", \"reserved_working_bytes_high_water\": "
               << summary.reserved_working_bytes_high_water
               << ", \"gates\": {\"evaluated\": " << (summary.gates.evaluated ? "true" : "false")
               << ", \"passed\": " << (summary.gates.passed ? "true" : "false")
               << ", \"violations\": [";
        for (std::size_t violation_index = 0; violation_index < summary.gates.violations.size();
             ++violation_index) {
            const auto& violation = summary.gates.violations[violation_index];
            output << (violation_index == 0 ? "" : ", ") << "{\"metric\": ";
            write_json_string(output, violation.metric);
            output << ", \"actual\": " << violation.actual << ", \"limit\": " << violation.limit
                   << '}';
        }
        output << "]}}" << (index + 1U == summary_values.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

core::Status ChunkStreamingBenchmarkReport::write_json(const std::filesystem::path& path) const {
    auto status = validate();
    if (!status) {
        return status;
    }
    return write_text_file(path, to_json());
}

core::Result<ChunkStreamingBenchmarkReport>
run_chunk_streaming_benchmark(const ChunkStreamingBenchmarkConfig& config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<ChunkStreamingBenchmarkReport>::failure(status.error().code,
                                                                    status.error().message);
    }
    ChunkStreamingBenchmarkReport report;
    report.config = config;
    report.runtime = profiling::query_runtime_metadata();

    for (const auto workload : config.workloads) {
        const auto total_passes = config.warmup_repetitions + config.repetitions;
        for (std::uint32_t pass = 0; pass < total_passes; ++pass) {
            const auto measured_repetition =
                pass < config.warmup_repetitions ? 0 : pass - config.warmup_repetitions;
            auto executed = run_workload(config, workload, measured_repetition);
            if (!executed) {
                return core::Result<ChunkStreamingBenchmarkReport>::failure(
                    executed.error().code, executed.error().message);
            }
            if (pass < config.warmup_repetitions) {
                continue;
            }
            report.runs.push_back(std::move(executed.value().run));
            report.raw_samples.insert(report.raw_samples.end(),
                                      std::make_move_iterator(executed.value().samples.begin()),
                                      std::make_move_iterator(executed.value().samples.end()));
        }
    }
    status = report.validate();
    if (!status) {
        return core::Result<ChunkStreamingBenchmarkReport>::failure(status.error().code,
                                                                    status.error().message);
    }
    return core::Result<ChunkStreamingBenchmarkReport>::success(std::move(report));
}

} // namespace heartstead::world::benchmark
