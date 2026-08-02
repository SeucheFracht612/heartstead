#include "engine/world/streaming/predictive_streaming_benchmark.hpp"

#include "engine/profiling/profiler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <thread>
#include <utility>

namespace heartstead::world::benchmark {

namespace {

using BenchmarkClock = std::chrono::steady_clock;

constexpr ChunkCoord benchmark_center{4'096, 0, -4'096};

struct MotionStep {
    PredictiveStreamingPhase phase = PredictiveStreamingPhase::steady_travel;
    ChunkCoord coord;
    std::int64_t direction_x = 0;
    std::int64_t direction_z = 0;
    ChunkStreamMemoryPressure pressure = ChunkStreamMemoryPressure::nominal;
    bool teleport = false;
};

[[nodiscard]] std::uint64_t elapsed_microseconds(BenchmarkClock::time_point begin,
                                                 BenchmarkClock::time_point end) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

[[nodiscard]] simulation::WorldTick elapsed_milliseconds(BenchmarkClock::time_point begin,
                                                         BenchmarkClock::time_point end) noexcept {
    return elapsed_microseconds(begin, end) / 1'000;
}

[[nodiscard]] std::optional<std::int64_t> checked_add(std::int64_t value,
                                                      std::int64_t offset) noexcept {
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if ((offset > 0 && value > maximum - offset) || (offset < 0 && value < minimum - offset)) {
        return std::nullopt;
    }
    return value + offset;
}

[[nodiscard]] core::Result<ChunkLoadSchedulerContext> make_benchmark_context(std::uint64_t seed) {
    const auto stone_id = core::PrototypeId::parse("benchmark:voxels/predictive_stone");
    if (!stone_id) {
        return core::Result<ChunkLoadSchedulerContext>::failure(
            "predictive_streaming_benchmark.invalid_voxel_id",
            "the predictive benchmark voxel id is invalid");
    }

    VoxelDefinition stone;
    stone.type = 1;
    stone.prototype_id = *stone_id;
    stone.display_name = "Predictive benchmark stone";
    stone.terrain_material = "predictive_benchmark_stone";
    stone.mining_tool = "pickaxe";

    RegionDescriptor region;
    region.id = "predictive_benchmark_region";
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

[[nodiscard]] core::Result<std::vector<MotionStep>>
build_motion_steps(const PredictiveStreamingBenchmarkConfig& config) {
    std::vector<MotionStep> result;
    result.reserve(config.movement_step_count());
    auto current = benchmark_center;

    const auto append = [&](PredictiveStreamingPhase phase, std::int64_t direction_x,
                            std::int64_t direction_z, ChunkStreamMemoryPressure pressure,
                            bool teleport) -> core::Status {
        const auto next_x = checked_add(current.x, direction_x);
        const auto next_z = checked_add(current.z, direction_z);
        if (!next_x || !next_z) {
            return core::Status::failure("predictive_streaming_benchmark.path_overflow",
                                         "benchmark traversal exceeds chunk coordinate range");
        }
        current.x = *next_x;
        current.z = *next_z;
        result.push_back({phase, current, direction_x, direction_z, pressure, teleport});
        return core::Status::ok();
    };

    for (std::uint32_t step = 0; step < config.steady_steps; ++step) {
        auto status = append(PredictiveStreamingPhase::steady_travel, 1, 0,
                             ChunkStreamMemoryPressure::nominal, false);
        if (!status) {
            return core::Result<std::vector<MotionStep>>::failure(status.error().code,
                                                                  status.error().message);
        }
    }
    for (std::uint32_t step = 0; step < config.reversal_steps; ++step) {
        auto status = append(PredictiveStreamingPhase::reversal, -1, 0,
                             ChunkStreamMemoryPressure::nominal, false);
        if (!status) {
            return core::Result<std::vector<MotionStep>>::failure(status.error().code,
                                                                  status.error().message);
        }
    }
    auto status =
        append(PredictiveStreamingPhase::teleport, config.teleport_distance_chunks,
               -config.teleport_distance_chunks, ChunkStreamMemoryPressure::critical, true);
    if (!status) {
        return core::Result<std::vector<MotionStep>>::failure(status.error().code,
                                                              status.error().message);
    }
    for (std::uint32_t step = 0; step < config.post_teleport_steps; ++step) {
        const auto pressure =
            step < 2 ? ChunkStreamMemoryPressure::elevated : ChunkStreamMemoryPressure::nominal;
        status = append(PredictiveStreamingPhase::post_teleport, 0, 1, pressure, false);
        if (!status) {
            return core::Result<std::vector<MotionStep>>::failure(status.error().code,
                                                                  status.error().message);
        }
    }
    for (std::uint32_t step = 0; step < config.soak_steps; ++step) {
        status = append(PredictiveStreamingPhase::bounded_soak, 0, 1,
                        ChunkStreamMemoryPressure::nominal, false);
        if (!status) {
            return core::Result<std::vector<MotionStep>>::failure(status.error().code,
                                                                  status.error().message);
        }
    }
    return core::Result<std::vector<MotionStep>>::success(std::move(result));
}

[[nodiscard]] core::Result<ChunkStreamViewerMotion>
viewer_motion(ChunkCoord coord, std::int64_t direction_x, std::int64_t direction_z, bool teleport,
              const PredictiveStreamingBenchmarkConfig& config) {
    auto block = chunk_local_to_block(coord, {0, 0, 0});
    if (!block) {
        return core::Result<ChunkStreamViewerMotion>::failure(block.error().code,
                                                              block.error().message);
    }
    const auto speed = static_cast<double>(chunk_edge_length) * 1'000'000.0 /
                       static_cast<double>(config.movement_interval_us);
    ChunkStreamViewerMotion motion;
    motion.viewer.viewer_id = core::NetId::from_value(1);
    motion.viewer.coord = block.value();
    motion.velocity_x_blocks_per_second = static_cast<double>(direction_x) * speed;
    motion.velocity_z_blocks_per_second = static_cast<double>(direction_z) * speed;
    motion.view_direction_x = static_cast<double>(direction_x);
    motion.view_direction_z = static_cast<double>(direction_z);
    motion.teleport = teleport;
    return core::Result<ChunkStreamViewerMotion>::success(motion);
}

[[nodiscard]] double percentile95_ms(std::vector<std::uint64_t> values_us) {
    if (values_us.empty()) {
        return 0.0;
    }
    std::ranges::sort(values_us);
    const auto rank = (values_us.size() * 95U + 99U) / 100U;
    const auto index = std::max<std::size_t>(1, rank) - 1;
    return static_cast<double>(values_us[index]) / 1'000.0;
}

[[nodiscard]] double memory_slope(std::span<const std::size_t> samples) noexcept {
    if (samples.size() < 2) {
        return 0.0;
    }
    const auto first = samples.size() / 2;
    const auto count = samples.size() - first;
    if (count < 2) {
        return 0.0;
    }
    const auto mean_x = static_cast<double>(count - 1) * 0.5;
    const auto sum_y =
        std::accumulate(samples.begin() + static_cast<std::ptrdiff_t>(first), samples.end(), 0.0);
    const auto mean_y = sum_y / static_cast<double>(count);
    auto numerator = 0.0;
    auto denominator = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto centered_x = static_cast<double>(index) - mean_x;
        const auto centered_y = static_cast<double>(samples[first + index]) - mean_y;
        numerator += centered_x * centered_y;
        denominator += centered_x * centered_x;
    }
    return denominator == 0.0 ? 0.0 : numerator / denominator;
}

class TrialRunner {
  public:
    TrialRunner(const PredictiveStreamingBenchmarkConfig& config, bool prediction_enabled)
        : config_(config), prediction_enabled_(prediction_enabled), policy_(config.policy) {
        if (!prediction_enabled_) {
            policy_.prediction_horizon_seconds = 0.0;
            policy_.camera_lookahead_chunks = 0.0;
        }
        trial_.prediction_enabled = prediction_enabled_;
    }

    [[nodiscard]] core::Result<PredictiveStreamingBenchmarkTrial> run() {
        HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.predictive_streaming.trial");
        auto context = make_benchmark_context(config_.seed);
        if (!context) {
            return core::Result<PredictiveStreamingBenchmarkTrial>::failure(
                context.error().code, context.error().message);
        }
        auto benchmark_context = std::move(context).value();
        auto warm =
            ChunkStreamer::load_chunk(state_, benchmark_center, benchmark_context.generation,
                                      benchmark_context.regions, benchmark_context.palette);
        if (!warm) {
            return core::Result<PredictiveStreamingBenchmarkTrial>::failure(warm.error().code,
                                                                            warm.error().message);
        }
        auto created = ChunkLoadScheduler::create(std::move(benchmark_context), config_.scheduler);
        if (!created) {
            return core::Result<PredictiveStreamingBenchmarkTrial>::failure(
                created.error().code, created.error().message);
        }
        scheduler_ = std::move(created).value();

        auto steps = build_motion_steps(config_);
        if (!steps) {
            return core::Result<PredictiveStreamingBenchmarkTrial>::failure(steps.error().code,
                                                                            steps.error().message);
        }
        trial_started_ = BenchmarkClock::now();
        auto priming = viewer_motion(benchmark_center, 1, 0, false, config_);
        if (!priming) {
            return core::Result<PredictiveStreamingBenchmarkTrial>::failure(
                priming.error().code, priming.error().message);
        }
        auto status = drive_for(priming.value(), ChunkStreamMemoryPressure::nominal,
                                config_.movement_interval_us);
        if (!status) {
            return core::Result<PredictiveStreamingBenchmarkTrial>::failure(status.error().code,
                                                                            status.error().message);
        }

        std::uint32_t ordinal = 0;
        auto previous_coord = benchmark_center;
        bool cancellation_probe_executed = false;
        for (const auto& step : steps.value()) {
            if (config_.exercise_cancellation_probe && !cancellation_probe_executed &&
                step.phase == PredictiveStreamingPhase::reversal) {
                auto probe = viewer_motion(previous_coord, 0, 1, false, config_);
                if (!probe) {
                    return core::Result<PredictiveStreamingBenchmarkTrial>::failure(
                        probe.error().code, probe.error().message);
                }
                status = update_once(probe.value(), ChunkStreamMemoryPressure::nominal);
                if (!status) {
                    return core::Result<PredictiveStreamingBenchmarkTrial>::failure(
                        status.error().code, status.error().message);
                }
                cancellation_probe_executed = true;
            }
            auto motion = viewer_motion(step.coord, step.direction_x, step.direction_z,
                                        step.teleport, config_);
            if (!motion) {
                return core::Result<PredictiveStreamingBenchmarkTrial>::failure(
                    motion.error().code, motion.error().message);
            }
            status = execute_step(step, motion.value(), ordinal++);
            if (!status) {
                return core::Result<PredictiveStreamingBenchmarkTrial>::failure(
                    status.error().code, status.error().message);
            }
            previous_coord = step.coord;
        }

        const auto final_coord =
            steps.value().empty() ? benchmark_center : steps.value().back().coord;
        auto final_motion = viewer_motion(final_coord, 0, 0, false, config_);
        if (!final_motion) {
            return core::Result<PredictiveStreamingBenchmarkTrial>::failure(
                final_motion.error().code, final_motion.error().message);
        }
        const auto settle_started = BenchmarkClock::now();
        while (scheduler_->has_in_flight() || controller_.has_pending_loads() ||
               controller_.stats().active_speculative_requests != 0 ||
               !state_.chunks().contains(final_coord)) {
            const auto now = BenchmarkClock::now();
            if (elapsed_milliseconds(settle_started, now) > config_.settle_timeout_ms) {
                return core::Result<PredictiveStreamingBenchmarkTrial>::failure(
                    "predictive_streaming_benchmark.settle_timeout",
                    "streaming work did not drain before the timeout");
            }
            status = update_once(final_motion.value(), ChunkStreamMemoryPressure::critical);
            if (!status) {
                return core::Result<PredictiveStreamingBenchmarkTrial>::failure(
                    status.error().code, status.error().message);
            }
            std::this_thread::sleep_for(
                std::chrono::microseconds(config_.owner_update_interval_us));
        }
        status = update_once(final_motion.value(), ChunkStreamMemoryPressure::critical);
        if (!status) {
            return core::Result<PredictiveStreamingBenchmarkTrial>::failure(status.error().code,
                                                                            status.error().message);
        }

        trial_.elapsed_us = elapsed_microseconds(trial_started_, BenchmarkClock::now());
        trial_.movement_steps = trial_.raw_steps.size();
        trial_.immediate_hit_rate = trial_.movement_steps == 0
                                        ? 0.0
                                        : static_cast<double>(trial_.immediate_required_hits) /
                                              static_cast<double>(trial_.movement_steps);
        trial_.p95_visible_hole_ms = percentile95_ms(hole_samples_us_);
        trial_.maximum_visible_hole_ms =
            hole_samples_us_.empty()
                ? 0.0
                : static_cast<double>(*std::ranges::max_element(hole_samples_us_)) / 1'000.0;
        trial_.final_resident_chunk_count = state_.chunks().stats().chunk_count;
        trial_.soak_memory_slope_chunks_per_step = memory_slope(soak_resident_samples_);
        trial_.policy_stats = controller_.stats();
        trial_.final_pending_load_count = controller_.pending_load_count();
        const auto& scheduler_stats = scheduler_->stats();
        trial_.scheduler_submitted_requests = scheduler_stats.submitted_requests;
        trial_.scheduler_published_requests = scheduler_stats.published_requests;
        trial_.scheduler_cancelled_requests = scheduler_stats.cancelled_requests;
        trial_.scheduler_failed_requests = scheduler_stats.failed_requests;
        trial_.scheduler_stale_requests = scheduler_stats.stale_requests;
        trial_.scheduler_rejected_requests = scheduler_stats.rejected_requests;
        trial_.scheduler_duplicate_requests = scheduler_stats.duplicate_requests;
        trial_.final_reserved_working_bytes = scheduler_stats.reserved_working_bytes;
        return core::Result<PredictiveStreamingBenchmarkTrial>::success(std::move(trial_));
    }

  private:
    [[nodiscard]] core::Status update_once(const ChunkStreamViewerMotion& motion,
                                           ChunkStreamMemoryPressure pressure) {
        const std::vector<ChunkStreamViewerMotion> viewers{motion};
        const auto now_ms = elapsed_milliseconds(trial_started_, BenchmarkClock::now());
        auto update = controller_.update(state_, *scheduler_, viewers, policy_, pressure, now_ms);
        if (!update) {
            return core::Status::failure(update.error().code, update.error().message);
        }
        trial_.evicted_chunks += update.value().eviction.evicted_count();
        trial_.deferred_required_loads += update.value().deferred_required_loads;
        trial_.maximum_owner_publication_us = std::max(
            trial_.maximum_owner_publication_us, update.value().publication.publication_time_us);
        trial_.maximum_resident_chunk_count =
            std::max(trial_.maximum_resident_chunk_count, state_.chunks().stats().chunk_count);
        return core::Status::ok();
    }

    [[nodiscard]] core::Status drive_for(const ChunkStreamViewerMotion& motion,
                                         ChunkStreamMemoryPressure pressure,
                                         std::uint64_t duration_us) {
        const auto started = BenchmarkClock::now();
        while (elapsed_microseconds(started, BenchmarkClock::now()) < duration_us) {
            auto status = update_once(motion, pressure);
            if (!status) {
                return status;
            }
            std::this_thread::sleep_for(
                std::chrono::microseconds(config_.owner_update_interval_us));
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Status execute_step(const MotionStep& step,
                                            const ChunkStreamViewerMotion& motion,
                                            std::uint32_t ordinal) {
        PredictiveStreamingStepSample sample;
        sample.prediction_enabled = prediction_enabled_;
        sample.phase = step.phase;
        sample.ordinal = ordinal;
        sample.coord = step.coord;
        sample.pressure = step.pressure;
        sample.required_resident_at_start = state_.chunks().contains(step.coord);
        if (sample.required_resident_at_start) {
            ++trial_.immediate_required_hits;
        }

        const auto started = BenchmarkClock::now();
        std::optional<std::uint64_t> became_resident_us;
        while (elapsed_microseconds(started, BenchmarkClock::now()) <
               config_.movement_interval_us) {
            auto status = update_once(motion, step.pressure);
            if (!status) {
                return status;
            }
            if (!sample.required_resident_at_start && !became_resident_us.has_value() &&
                state_.chunks().contains(step.coord)) {
                became_resident_us = elapsed_microseconds(started, BenchmarkClock::now());
            }
            std::this_thread::sleep_for(
                std::chrono::microseconds(config_.owner_update_interval_us));
        }
        sample.required_resident_before_deadline = state_.chunks().contains(step.coord);
        if (sample.required_resident_at_start) {
            sample.visible_hole_us = 0;
        } else if (became_resident_us.has_value()) {
            sample.visible_hole_us = *became_resident_us;
        } else {
            sample.visible_hole_us = elapsed_microseconds(started, BenchmarkClock::now());
        }
        if (sample.visible_hole_us != 0) {
            ++trial_.steps_with_visible_holes;
        }
        hole_samples_us_.push_back(sample.visible_hole_us);
        sample.resident_chunk_count = state_.chunks().stats().chunk_count;
        sample.pending_load_count = controller_.pending_load_count();
        sample.active_speculative_count = controller_.stats().active_speculative_requests;
        sample.cumulative_evicted_chunks = trial_.evicted_chunks;
        if (step.phase == PredictiveStreamingPhase::bounded_soak) {
            soak_resident_samples_.push_back(sample.resident_chunk_count);
        }
        trial_.raw_steps.push_back(sample);
        return core::Status::ok();
    }

    const PredictiveStreamingBenchmarkConfig& config_;
    bool prediction_enabled_ = false;
    PredictiveChunkStreamingPolicy policy_;
    WorldState state_;
    PredictiveChunkStreamingController controller_;
    std::unique_ptr<ChunkLoadScheduler> scheduler_;
    PredictiveStreamingBenchmarkTrial trial_;
    BenchmarkClock::time_point trial_started_{};
    std::vector<std::uint64_t> hole_samples_us_;
    std::vector<std::size_t> soak_resident_samples_;
};

[[nodiscard]] bool finite_non_negative(double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

void add_upper_violation(PredictiveStreamingBenchmarkGateEvaluation& gates, std::string metric,
                         double actual, double limit) {
    if (actual <= limit) {
        return;
    }
    gates.passed = false;
    gates.violations.push_back({std::move(metric), actual, limit});
}

void add_lower_violation(PredictiveStreamingBenchmarkGateEvaluation& gates, std::string metric,
                         double actual, double limit) {
    if (actual >= limit) {
        return;
    }
    gates.passed = false;
    gates.violations.push_back({std::move(metric), actual, limit});
}

[[nodiscard]] PredictiveStreamingBenchmarkGateEvaluation
evaluate_gates(const PredictiveStreamingBenchmarkConfig& config,
               const PredictiveStreamingBenchmarkTrial& baseline,
               const PredictiveStreamingBenchmarkTrial& predictive) {
    PredictiveStreamingBenchmarkGateEvaluation gates;
    gates.evaluated = true;

    const auto resolved =
        predictive.policy_stats.useful_predictions + predictive.policy_stats.wasted_predictions;
    const auto waste_ratio = resolved == 0
                                 ? 1.0
                                 : static_cast<double>(predictive.policy_stats.wasted_predictions) /
                                       static_cast<double>(resolved);
    const auto cancellation_ratio =
        predictive.policy_stats.cancellation_requests == 0
            ? 0.0
            : static_cast<double>(predictive.policy_stats.cancelled_requests) /
                  static_cast<double>(predictive.policy_stats.cancellation_requests);
    const auto baseline_hole_rate = baseline.movement_steps == 0
                                        ? 0.0
                                        : static_cast<double>(baseline.steps_with_visible_holes) /
                                              static_cast<double>(baseline.movement_steps);
    const auto predictive_hole_rate =
        predictive.movement_steps == 0 ? 0.0
                                       : static_cast<double>(predictive.steps_with_visible_holes) /
                                             static_cast<double>(predictive.movement_steps);
    const auto hole_rate_ratio =
        baseline_hole_rate == 0.0
            ? (predictive_hole_rate == 0.0 ? 0.0 : std::numeric_limits<double>::max())
            : predictive_hole_rate / baseline_hole_rate;

    add_upper_violation(gates, "predictive_visible_hole_p95_ms", predictive.p95_visible_hole_ms,
                        config.maximum_predictive_hole_p95_ms);
    add_lower_violation(gates, "predictive_immediate_hit_rate", predictive.immediate_hit_rate,
                        config.minimum_predictive_immediate_hit_rate);
    add_lower_violation(gates, "prediction_accuracy", predictive.policy_stats.prediction_accuracy,
                        config.minimum_prediction_accuracy);
    add_upper_violation(gates, "prediction_waste_ratio", waste_ratio,
                        config.maximum_prediction_waste_ratio);
    add_lower_violation(gates, "cancellation_completion_ratio", cancellation_ratio,
                        config.minimum_cancellation_completion_ratio);
    add_upper_violation(gates, "predictive_hole_rate_ratio_vs_baseline", hole_rate_ratio,
                        config.maximum_predictive_hole_rate_ratio_vs_baseline);
    add_upper_violation(gates, "soak_memory_slope_chunks_per_step",
                        predictive.soak_memory_slope_chunks_per_step,
                        config.maximum_soak_memory_slope_chunks_per_step);
    add_upper_violation(gates, "owner_publication_us",
                        static_cast<double>(predictive.maximum_owner_publication_us),
                        static_cast<double>(config.maximum_owner_publication_us));
    return gates;
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"';
    for (const char character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            output << character;
            break;
        }
    }
    output << '"';
}

void write_policy_stats(std::ostream& output, const PredictiveChunkStreamingStats& stats,
                        std::string_view indent) {
    output << indent << "{\n";
    output << indent << "  \"planning_updates\": " << stats.planning_updates << ",\n";
    output << indent << "  \"teleport_updates\": " << stats.teleport_updates << ",\n";
    output << indent << "  \"demand_transitions\": " << stats.demand_transitions << ",\n";
    output << indent << "  \"speculative_submissions\": " << stats.speculative_submissions << ",\n";
    output << indent << "  \"speculative_publications\": " << stats.speculative_publications
           << ",\n";
    output << indent << "  \"useful_predictions\": " << stats.useful_predictions << ",\n";
    output << indent << "  \"timely_prefetch_hits\": " << stats.timely_prefetch_hits << ",\n";
    output << indent << "  \"late_prefetch_hits\": " << stats.late_prefetch_hits << ",\n";
    output << indent << "  \"wasted_predictions\": " << stats.wasted_predictions << ",\n";
    output << indent << "  \"cancellation_requests\": " << stats.cancellation_requests << ",\n";
    output << indent << "  \"cancelled_requests\": " << stats.cancelled_requests << ",\n";
    output << indent << "  \"cancellation_misses\": " << stats.cancellation_misses << ",\n";
    output << indent << "  \"failed_requests\": " << stats.failed_requests << ",\n";
    output << indent << "  \"stale_results\": " << stats.stale_results << ",\n";
    output << indent << "  \"cumulative_timeliness_ms\": " << stats.cumulative_timeliness_ms
           << ",\n";
    output << indent << "  \"active_speculative_requests\": " << stats.active_speculative_requests
           << ",\n";
    output << indent << "  \"prediction_accuracy\": " << stats.prediction_accuracy << ",\n";
    output << indent << "  \"timely_coverage\": " << stats.timely_coverage << ",\n";
    output << indent << "  \"mean_timeliness_ms\": " << stats.mean_timeliness_ms << ",\n";
    output << indent << "  \"maximum_timeliness_ms\": " << stats.maximum_timeliness_ms << "\n";
    output << indent << '}';
}

void write_trial(std::ostream& output, const PredictiveStreamingBenchmarkTrial& trial,
                 std::string_view indent) {
    output << indent << "{\n";
    output << indent
           << "  \"prediction_enabled\": " << (trial.prediction_enabled ? "true" : "false")
           << ",\n";
    output << indent << "  \"elapsed_us\": " << trial.elapsed_us << ",\n";
    output << indent << "  \"movement_steps\": " << trial.movement_steps << ",\n";
    output << indent << "  \"immediate_required_hits\": " << trial.immediate_required_hits << ",\n";
    output << indent << "  \"steps_with_visible_holes\": " << trial.steps_with_visible_holes
           << ",\n";
    output << indent << "  \"immediate_hit_rate\": " << trial.immediate_hit_rate << ",\n";
    output << indent << "  \"p95_visible_hole_ms\": " << trial.p95_visible_hole_ms << ",\n";
    output << indent << "  \"maximum_visible_hole_ms\": " << trial.maximum_visible_hole_ms << ",\n";
    output << indent << "  \"maximum_resident_chunk_count\": " << trial.maximum_resident_chunk_count
           << ",\n";
    output << indent << "  \"final_resident_chunk_count\": " << trial.final_resident_chunk_count
           << ",\n";
    output << indent
           << "  \"soak_memory_slope_chunks_per_step\": " << trial.soak_memory_slope_chunks_per_step
           << ",\n";
    output << indent << "  \"evicted_chunks\": " << trial.evicted_chunks << ",\n";
    output << indent << "  \"deferred_required_loads\": " << trial.deferred_required_loads << ",\n";
    output << indent << "  \"maximum_owner_publication_us\": " << trial.maximum_owner_publication_us
           << ",\n";
    output << indent << "  \"scheduler_submitted_requests\": " << trial.scheduler_submitted_requests
           << ",\n";
    output << indent << "  \"scheduler_published_requests\": " << trial.scheduler_published_requests
           << ",\n";
    output << indent << "  \"scheduler_cancelled_requests\": " << trial.scheduler_cancelled_requests
           << ",\n";
    output << indent << "  \"scheduler_failed_requests\": " << trial.scheduler_failed_requests
           << ",\n";
    output << indent << "  \"scheduler_stale_requests\": " << trial.scheduler_stale_requests
           << ",\n";
    output << indent << "  \"scheduler_rejected_requests\": " << trial.scheduler_rejected_requests
           << ",\n";
    output << indent << "  \"scheduler_duplicate_requests\": " << trial.scheduler_duplicate_requests
           << ",\n";
    output << indent << "  \"final_pending_load_count\": " << trial.final_pending_load_count
           << ",\n";
    output << indent << "  \"final_reserved_working_bytes\": " << trial.final_reserved_working_bytes
           << ",\n";
    output << indent << "  \"policy_stats\": ";
    write_policy_stats(output, trial.policy_stats, std::string(indent) + "  ");
    output << ",\n";
    output << indent << "  \"raw_steps\": [\n";
    for (std::size_t index = 0; index < trial.raw_steps.size(); ++index) {
        const auto& sample = trial.raw_steps[index];
        output << indent << "    {\"phase\": ";
        write_json_string(output, predictive_streaming_phase_name(sample.phase));
        output << ", \"ordinal\": " << sample.ordinal << ", \"coord\": [" << sample.coord.x << ", "
               << sample.coord.y << ", " << sample.coord.z << "], \"pressure\": ";
        write_json_string(output, chunk_stream_memory_pressure_name(sample.pressure));
        output << ", \"required_resident_at_start\": "
               << (sample.required_resident_at_start ? "true" : "false")
               << ", \"required_resident_before_deadline\": "
               << (sample.required_resident_before_deadline ? "true" : "false")
               << ", \"visible_hole_us\": " << sample.visible_hole_us
               << ", \"resident_chunk_count\": " << sample.resident_chunk_count
               << ", \"pending_load_count\": " << sample.pending_load_count
               << ", \"active_speculative_count\": " << sample.active_speculative_count
               << ", \"cumulative_evicted_chunks\": " << sample.cumulative_evicted_chunks << '}';
        output << (index + 1 == trial.raw_steps.size() ? "\n" : ",\n");
    }
    output << indent << "  ]\n";
    output << indent << '}';
}

[[nodiscard]] core::Status write_text_file(const std::filesystem::path& path,
                                           std::string_view text) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return core::Status::failure("predictive_streaming_benchmark.create_directory_failed",
                                         error.message());
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Status::failure("predictive_streaming_benchmark.open_output_failed",
                                     "failed to open predictive streaming benchmark output");
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        return core::Status::failure("predictive_streaming_benchmark.write_output_failed",
                                     "failed to write predictive streaming benchmark output");
    }
    return core::Status::ok();
}

} // namespace

std::string_view predictive_streaming_phase_name(PredictiveStreamingPhase phase) noexcept {
    switch (phase) {
    case PredictiveStreamingPhase::steady_travel:
        return "steady_travel";
    case PredictiveStreamingPhase::reversal:
        return "reversal";
    case PredictiveStreamingPhase::teleport:
        return "teleport";
    case PredictiveStreamingPhase::post_teleport:
        return "post_teleport";
    case PredictiveStreamingPhase::bounded_soak:
        return "bounded_soak";
    }
    return "unknown";
}

PredictiveStreamingBenchmarkConfig::PredictiveStreamingBenchmarkConfig() {
    policy.interest.load_horizontal_radius_chunks = 0;
    policy.interest.load_vertical_radius_chunks = 0;
    policy.interest.retain_horizontal_radius_chunks = 2;
    policy.interest.retain_vertical_radius_chunks = 0;
    policy.prediction_horizon_seconds = 1.0;
    policy.camera_lookahead_chunks = 1.0;
    policy.max_prediction_distance_chunks = 4;
    policy.predictive_horizontal_radius_chunks = 0;
    policy.predictive_vertical_radius_chunks = 0;
    policy.max_speculative_submissions_per_update = 4;
    policy.max_active_speculative_requests = 8;
    policy.reserved_required_request_slots = 1;
    policy.speculative_ttl_ms = 500;
    policy.temporal_retention_ms = 500;
    policy.nominal_resident_chunk_budget = 16;
    policy.elevated_resident_chunk_budget = 8;
    policy.critical_resident_chunk_budget = 4;

    scheduler.worker_count = 2;
    scheduler.max_concurrent_requests = 8;
    scheduler.max_completed_results = 8;
    scheduler.max_reserved_working_bytes =
        scheduler.reservation_bytes_per_request * scheduler.max_concurrent_requests;
    scheduler.max_publications_per_update = 4;
    scheduler.max_publication_time_us = maximum_owner_publication_us;
}

std::size_t PredictiveStreamingBenchmarkConfig::movement_step_count() const noexcept {
    return static_cast<std::size_t>(steady_steps) + static_cast<std::size_t>(reversal_steps) + 1U +
           static_cast<std::size_t>(post_teleport_steps) + static_cast<std::size_t>(soak_steps);
}

core::Status PredictiveStreamingBenchmarkConfig::validate() const {
    auto status = policy.validate();
    if (!status) {
        return status;
    }
    status = scheduler.validate();
    if (!status) {
        return status;
    }
    if (steady_steps == 0 || reversal_steps == 0 || post_teleport_steps == 0 || soak_steps < 2 ||
        teleport_distance_chunks == 0 || movement_interval_us == 0 ||
        owner_update_interval_us == 0 || owner_update_interval_us > movement_interval_us ||
        settle_timeout_ms == 0) {
        return core::Status::failure("predictive_streaming_benchmark.invalid_workload",
                                     "travel phases, teleport distance, cadence, and timeout must "
                                     "define a bounded workload");
    }
    if (policy.reserved_required_request_slots >= scheduler.max_concurrent_requests) {
        return core::Status::failure(
            "predictive_streaming_benchmark.invalid_required_reserve",
            "required request reserve must leave at least one scheduler slot for speculation");
    }
    constexpr auto maximum_path_component = 1'000'000;
    if (teleport_distance_chunks < -maximum_path_component ||
        teleport_distance_chunks > maximum_path_component ||
        movement_step_count() > static_cast<std::size_t>(maximum_path_component)) {
        return core::Status::failure("predictive_streaming_benchmark.path_too_large",
                                     "benchmark path exceeds its deterministic safety bound");
    }
    if (!finite_non_negative(maximum_predictive_hole_p95_ms) ||
        !finite_non_negative(minimum_predictive_immediate_hit_rate) ||
        !finite_non_negative(minimum_prediction_accuracy) ||
        !finite_non_negative(maximum_prediction_waste_ratio) ||
        !finite_non_negative(minimum_cancellation_completion_ratio) ||
        !finite_non_negative(maximum_predictive_hole_rate_ratio_vs_baseline) ||
        !finite_non_negative(maximum_soak_memory_slope_chunks_per_step) ||
        maximum_owner_publication_us == 0 || minimum_predictive_immediate_hit_rate > 1.0 ||
        minimum_prediction_accuracy > 1.0 || maximum_prediction_waste_ratio > 1.0 ||
        minimum_cancellation_completion_ratio > 1.0) {
        return core::Status::failure("predictive_streaming_benchmark.invalid_gates",
                                     "predictive streaming gates are invalid");
    }
    return core::Status::ok();
}

core::Status PredictiveStreamingBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    const auto validate_trial = [&](const PredictiveStreamingBenchmarkTrial& trial,
                                    bool prediction_enabled) -> core::Status {
        if (trial.prediction_enabled != prediction_enabled ||
            trial.movement_steps != config.movement_step_count() ||
            trial.raw_steps.size() != config.movement_step_count()) {
            return core::Status::failure("predictive_streaming_benchmark.incomplete_trial",
                                         "benchmark trial does not contain every movement step");
        }
        if (trial.scheduler_failed_requests != 0 || trial.scheduler_stale_requests != 0 ||
            trial.scheduler_rejected_requests != 0 || trial.scheduler_duplicate_requests != 0 ||
            trial.final_pending_load_count != 0 || trial.final_reserved_working_bytes != 0 ||
            trial.policy_stats.active_speculative_requests != 0) {
            return core::Status::failure("predictive_streaming_benchmark.invalid_scheduler_state",
                                         "benchmark trial retained a failure, stale result, "
                                         "rejection, duplicate, or work reservation");
        }
        if (trial.scheduler_submitted_requests !=
            trial.scheduler_published_requests + trial.scheduler_cancelled_requests) {
            return core::Status::failure(
                "predictive_streaming_benchmark.incomplete_scheduler_accounting",
                "submitted chunk work did not end in publication or cancellation");
        }
        if (!std::isfinite(trial.immediate_hit_rate) || !std::isfinite(trial.p95_visible_hole_ms) ||
            !std::isfinite(trial.maximum_visible_hole_ms) ||
            !std::isfinite(trial.soak_memory_slope_chunks_per_step) ||
            trial.immediate_hit_rate < 0.0 || trial.immediate_hit_rate > 1.0 ||
            trial.final_resident_chunk_count > config.policy.critical_resident_chunk_budget) {
            return core::Status::failure(
                "predictive_streaming_benchmark.invalid_metrics",
                "benchmark trial contains invalid latency or residency metrics");
        }
        for (std::size_t index = 0; index < trial.raw_steps.size(); ++index) {
            const auto& sample = trial.raw_steps[index];
            if (sample.prediction_enabled != prediction_enabled || sample.ordinal != index) {
                return core::Status::failure("predictive_streaming_benchmark.invalid_sample",
                                             "raw movement sample provenance is invalid");
            }
        }
        return core::Status::ok();
    };
    status = validate_trial(baseline, false);
    if (!status) {
        return status;
    }
    status = validate_trial(predictive, true);
    if (!status) {
        return status;
    }
    if (baseline.policy_stats.speculative_submissions != 0 ||
        baseline.policy_stats.speculative_publications != 0 ||
        predictive.policy_stats.speculative_submissions == 0 ||
        predictive.policy_stats.demand_transitions == 0) {
        return core::Status::failure(
            "predictive_streaming_benchmark.missing_policy_evidence",
            "paired trials did not distinguish baseline and predictive policy behavior");
    }
    if (!gates.evaluated) {
        return core::Status::failure("predictive_streaming_benchmark.gates_not_evaluated",
                                     "benchmark report is missing its gate evaluation");
    }
    const auto expected_gate_pass = gates.violations.empty();
    if (gates.passed != expected_gate_pass) {
        return core::Status::failure("predictive_streaming_benchmark.invalid_gate_evaluation",
                                     "gate pass state disagrees with retained violations");
    }
    return core::Status::ok();
}

bool PredictiveStreamingBenchmarkReport::gates_passed() const noexcept {
    return gates.evaluated && gates.passed;
}

std::string PredictiveStreamingBenchmarkReport::to_json() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n";
    output << "  \"schema_version\": " << schema_version << ",\n";
    output << "  \"benchmark\": \"predictive_streaming\",\n";
    output << "  \"runtime\": {\n";
    output << "    \"engine_version\": ";
    write_json_string(output, runtime.engine_version);
    output << ",\n    \"git_commit\": ";
    write_json_string(output, runtime.git_commit);
    output << ",\n    \"git_dirty\": " << (runtime.git_dirty ? "true" : "false")
           << ",\n    \"build_configuration\": ";
    write_json_string(output, runtime.build_configuration);
    output << ",\n    \"compiler\": ";
    write_json_string(output, runtime.compiler);
    output << ",\n    \"platform\": ";
    write_json_string(output, runtime.platform);
    output << ",\n    \"architecture\": ";
    write_json_string(output, runtime.architecture);
    output << ",\n    \"operating_system\": ";
    write_json_string(output, runtime.operating_system);
    output << ",\n    \"cpu_model\": ";
    write_json_string(output, runtime.cpu_model);
    output << ",\n    \"logical_cpu_count\": " << runtime.logical_cpu_count
           << ",\n    \"tracy_enabled\": " << (runtime.tracy_enabled ? "true" : "false")
           << "\n  },\n";
    output << "  \"config\": {\n";
    output << "    \"seed\": " << config.seed << ",\n";
    output << "    \"steady_steps\": " << config.steady_steps << ",\n";
    output << "    \"reversal_steps\": " << config.reversal_steps << ",\n";
    output << "    \"post_teleport_steps\": " << config.post_teleport_steps << ",\n";
    output << "    \"soak_steps\": " << config.soak_steps << ",\n";
    output << "    \"teleport_distance_chunks\": " << config.teleport_distance_chunks << ",\n";
    output << "    \"movement_interval_us\": " << config.movement_interval_us << ",\n";
    output << "    \"owner_update_interval_us\": " << config.owner_update_interval_us << ",\n";
    output << "    \"settle_timeout_ms\": " << config.settle_timeout_ms << ",\n";
    output << "    \"exercise_cancellation_probe\": "
           << (config.exercise_cancellation_probe ? "true" : "false") << ",\n";
    output << "    \"worker_count\": " << config.scheduler.worker_count << ",\n";
    output << "    \"max_concurrent_requests\": " << config.scheduler.max_concurrent_requests
           << ",\n";
    output << "    \"max_publications_per_update\": "
           << config.scheduler.max_publications_per_update << ",\n";
    output << "    \"max_publication_time_us\": " << config.scheduler.max_publication_time_us
           << ",\n";
    output << "    \"prediction_horizon_seconds\": " << config.policy.prediction_horizon_seconds
           << ",\n";
    output << "    \"camera_lookahead_chunks\": " << config.policy.camera_lookahead_chunks << ",\n";
    output << "    \"max_prediction_distance_chunks\": "
           << config.policy.max_prediction_distance_chunks << ",\n";
    output << "    \"max_speculative_submissions_per_update\": "
           << config.policy.max_speculative_submissions_per_update << ",\n";
    output << "    \"max_active_speculative_requests\": "
           << config.policy.max_active_speculative_requests << ",\n";
    output << "    \"reserved_required_request_slots\": "
           << config.policy.reserved_required_request_slots << ",\n";
    output << "    \"speculative_ttl_ms\": " << config.policy.speculative_ttl_ms << ",\n";
    output << "    \"temporal_retention_ms\": " << config.policy.temporal_retention_ms << ",\n";
    output << "    \"resident_chunk_budgets\": [" << config.policy.nominal_resident_chunk_budget
           << ", " << config.policy.elevated_resident_chunk_budget << ", "
           << config.policy.critical_resident_chunk_budget << "],\n";
    output << "    \"enforce_gates\": " << (config.enforce_gates ? "true" : "false") << ",\n";
    output << "    \"maximum_predictive_hole_p95_ms\": " << config.maximum_predictive_hole_p95_ms
           << ",\n";
    output << "    \"minimum_predictive_immediate_hit_rate\": "
           << config.minimum_predictive_immediate_hit_rate << ",\n";
    output << "    \"minimum_prediction_accuracy\": " << config.minimum_prediction_accuracy
           << ",\n";
    output << "    \"maximum_prediction_waste_ratio\": " << config.maximum_prediction_waste_ratio
           << ",\n";
    output << "    \"minimum_cancellation_completion_ratio\": "
           << config.minimum_cancellation_completion_ratio << ",\n";
    output << "    \"maximum_predictive_hole_rate_ratio_vs_baseline\": "
           << config.maximum_predictive_hole_rate_ratio_vs_baseline << ",\n";
    output << "    \"maximum_soak_memory_slope_chunks_per_step\": "
           << config.maximum_soak_memory_slope_chunks_per_step << ",\n";
    output << "    \"maximum_owner_publication_us\": " << config.maximum_owner_publication_us
           << "\n  },\n";
    output << "  \"baseline\": ";
    write_trial(output, baseline, "  ");
    output << ",\n  \"predictive\": ";
    write_trial(output, predictive, "  ");
    output << ",\n  \"gates\": {\n";
    output << "    \"evaluated\": " << (gates.evaluated ? "true" : "false") << ",\n";
    output << "    \"passed\": " << (gates.passed ? "true" : "false") << ",\n";
    output << "    \"violations\": [";
    if (!gates.violations.empty()) {
        output << '\n';
    }
    for (std::size_t index = 0; index < gates.violations.size(); ++index) {
        const auto& violation = gates.violations[index];
        output << "      {\"metric\": ";
        write_json_string(output, violation.metric);
        output << ", \"actual\": " << violation.actual << ", \"limit\": " << violation.limit << '}';
        output << (index + 1 == gates.violations.size() ? "\n" : ",\n");
    }
    output << "    ]\n  }\n}\n";
    return output.str();
}

core::Status
PredictiveStreamingBenchmarkReport::write_json(const std::filesystem::path& path) const {
    auto status = validate();
    if (!status) {
        return status;
    }
    return write_text_file(path, to_json());
}

core::Result<PredictiveStreamingBenchmarkReport>
run_predictive_streaming_benchmark(const PredictiveStreamingBenchmarkConfig& config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<PredictiveStreamingBenchmarkReport>::failure(status.error().code,
                                                                         status.error().message);
    }
    PredictiveStreamingBenchmarkReport report;
    report.config = config;
    report.runtime = profiling::query_runtime_metadata();

    TrialRunner baseline(config, false);
    auto baseline_result = baseline.run();
    if (!baseline_result) {
        return core::Result<PredictiveStreamingBenchmarkReport>::failure(
            baseline_result.error().code, baseline_result.error().message);
    }
    report.baseline = std::move(baseline_result).value();

    TrialRunner predictive(config, true);
    auto predictive_result = predictive.run();
    if (!predictive_result) {
        return core::Result<PredictiveStreamingBenchmarkReport>::failure(
            predictive_result.error().code, predictive_result.error().message);
    }
    report.predictive = std::move(predictive_result).value();
    report.gates = evaluate_gates(config, report.baseline, report.predictive);

    status = report.validate();
    if (!status) {
        return core::Result<PredictiveStreamingBenchmarkReport>::failure(status.error().code,
                                                                         status.error().message);
    }
    return core::Result<PredictiveStreamingBenchmarkReport>::success(std::move(report));
}

} // namespace heartstead::world::benchmark
