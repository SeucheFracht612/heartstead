#include "engine/processes/process_temporal_aggregation_benchmark.hpp"

#include "engine/profiling/profiler.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace heartstead::processes::benchmark {

namespace {

constexpr std::uint32_t maximum_process_count = 1'000'000;
constexpr simulation::WorldTick maximum_simulation_ticks = 1'000'000;
constexpr std::uint64_t checksum_prime = 1'099'511'628'211ULL;

[[nodiscard]] const core::PrototypeId& benchmark_process_prototype() {
    static const auto prototype = *core::PrototypeId::parse("benchmark:processes/temporal");
    return prototype;
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint64_t combine_checksum(std::uint64_t current, std::uint64_t value) noexcept {
    return (current * checksum_prime) ^ value;
}

template <typename Function> [[nodiscard]] std::uint64_t measure_nanoseconds(Function&& function) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const auto started = std::chrono::steady_clock::now();
    function();
    const auto finished = std::chrono::steady_clock::now();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
    return elapsed <= 0 ? 1 : static_cast<std::uint64_t>(elapsed);
}

[[nodiscard]] bool is_stalled_process(const ProcessTemporalAggregationBenchmarkConfig& config,
                                      core::ProcessId process_id) noexcept {
    const auto first = static_cast<std::uint64_t>(config.burst_process_count) + 1U;
    const auto end = first + config.stalled_process_count;
    return process_id.value() >= first && process_id.value() < end;
}

[[nodiscard]] std::uint64_t active_rate_multiplier(std::uint64_t seed,
                                                   core::ProcessId process_id) noexcept {
    const auto exponent = splitmix64(seed ^ process_id.value()) % 3U;
    return std::uint64_t{1} << exponent;
}

[[nodiscard]] ProcessModifiers
modifiers_for(const ProcessTemporalAggregationBenchmarkConfig& config,
              const ProcessInstance& process) noexcept {
    ProcessModifiers modifiers;
    modifiers.quality_rate_per_mille =
        is_stalled_process(config, process.process_id)
            ? 0
            : static_cast<std::int64_t>(active_rate_multiplier(config.seed, process.process_id) *
                                        1'000U);
    return modifiers;
}

[[nodiscard]] core::Result<std::vector<ProcessInstance>>
build_corpus(const ProcessTemporalAggregationBenchmarkConfig& config) {
    std::vector<ProcessInstance> result;
    result.reserve(config.process_count);
    for (std::uint64_t value = 1; value <= config.process_count; ++value) {
        const auto process_id = core::ProcessId::from_value(value);
        const auto owner_id = core::SaveId::from_value(1'000'000U + value);
        simulation::WorldTick due_tick = config.stress_tick;
        if (value > config.burst_process_count) {
            due_tick = 1U + splitmix64(config.seed + value) % config.simulation_ticks;
        }
        const auto multiplier = active_rate_multiplier(config.seed, process_id);
        const auto required_work = is_stalled_process(config, process_id)
                                       ? config.simulation_ticks * 4U + 1U
                                       : due_tick * static_cast<simulation::WorldTick>(multiplier);
        auto process = ProcessRuntime::create(process_id, owner_id, benchmark_process_prototype(),
                                              0, required_work);
        if (!process) {
            return core::Result<std::vector<ProcessInstance>>::failure(process.error().code,
                                                                       process.error().message);
        }
        result.push_back(std::move(process).value());
    }
    return core::Result<std::vector<ProcessInstance>>::success(std::move(result));
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

[[nodiscard]] ProcessTemporalAggregationTimingSummary
summarize_nanoseconds(std::vector<std::uint64_t> values) {
    ProcessTemporalAggregationTimingSummary result;
    if (values.empty()) {
        return result;
    }
    std::vector<double> milliseconds;
    milliseconds.reserve(values.size());
    for (const auto value : values) {
        milliseconds.push_back(static_cast<double>(value) / 1'000'000.0);
    }
    std::ranges::sort(milliseconds);
    result.sample_count = milliseconds.size();
    result.minimum_ms = milliseconds.front();
    result.median_ms = percentile(milliseconds, 0.50);
    result.p95_ms = percentile(milliseconds, 0.95);
    result.p99_ms = percentile(milliseconds, 0.99);
    result.maximum_ms = milliseconds.back();
    result.mean_ms = std::accumulate(milliseconds.begin(), milliseconds.end(), 0.0) /
                     static_cast<double>(milliseconds.size());
    double squared_deviation = 0.0;
    for (const auto value : milliseconds) {
        const auto deviation = value - result.mean_ms;
        squared_deviation += deviation * deviation;
    }
    result.standard_deviation_ms =
        std::sqrt(squared_deviation / static_cast<double>(milliseconds.size()));
    result.coefficient_of_variation =
        result.mean_ms == 0.0 ? 0.0 : result.standard_deviation_ms / result.mean_ms;
    return result;
}

void observe_temporal_stats(const ProcessTemporalAggregationBenchmarkConfig& config,
                            const ProcessTemporalAggregationTickStats& stats,
                            ProcessTemporalAggregationBenchmarkRepetition& repetition,
                            std::uint32_t& backlog_streak, bool logical_tick) {
    repetition.maximum_active_event_count =
        std::max(repetition.maximum_active_event_count, stats.active_event_count);
    repetition.maximum_unadmitted_process_count =
        std::max(repetition.maximum_unadmitted_process_count, stats.unadmitted_process_count);
    repetition.maximum_deferred_lateness_ticks =
        std::max(repetition.maximum_deferred_lateness_ticks, stats.oldest_deferred_lateness_ticks);
    repetition.maximum_event_lateness_ticks =
        std::max(repetition.maximum_event_lateness_ticks, stats.maximum_lateness_ticks);
    repetition.unexpected_outcome_count += stats.stale_event_count;
    repetition.unexpected_outcome_count += stats.retired_event_count;
    if (logical_tick && stats.event_budget_exhausted) {
        ++backlog_streak;
        repetition.maximum_event_backlog_ticks =
            std::max(repetition.maximum_event_backlog_ticks, backlog_streak);
    } else if (logical_tick) {
        backlog_streak = 0;
    }

    const auto budget_valid =
        stats.admission_count <= config.temporal.maximum_admissions_per_tick &&
        stats.dispatched_event_count <= config.temporal.maximum_events_per_tick &&
        stats.evaluated_process_count <= stats.dispatched_event_count &&
        stats.catch_up_delta_ticks <= config.temporal.maximum_catch_up_ticks_per_tick &&
        stats.admitted_process_count <= config.temporal.maximum_tracked_processes &&
        stats.active_event_count <= config.temporal.maximum_tracked_processes &&
        stats.transitions.size() == stats.changed_process_count && !stats.counters_saturated;
    repetition.budget_violation_count += budget_valid ? 0U : 1U;
}

[[nodiscard]] ProcessTemporalAggregationBenchmarkSample
make_sample(std::uint32_t repetition, simulation::WorldTick world_tick, std::uint32_t drain_pass,
            std::uint64_t temporal_elapsed, std::uint64_t dense_elapsed,
            std::uint64_t temporal_resolver_calls, std::uint64_t dense_resolver_calls,
            const ProcessTemporalAggregationTickStats& stats) {
    return ProcessTemporalAggregationBenchmarkSample{
        .repetition = repetition,
        .world_tick = world_tick,
        .drain_pass = drain_pass,
        .temporal_elapsed_nanoseconds = temporal_elapsed,
        .dense_elapsed_nanoseconds = dense_elapsed,
        .temporal_resolver_call_count = static_cast<std::uint32_t>(temporal_resolver_calls),
        .dense_resolver_call_count = static_cast<std::uint32_t>(dense_resolver_calls),
        .temporal_admission_count = stats.admission_count,
        .temporal_dispatched_event_count = stats.dispatched_event_count,
        .temporal_evaluated_process_count = stats.evaluated_process_count,
        .temporal_changed_process_count = stats.changed_process_count,
        .temporal_completed_process_count = stats.completed_process_count,
        .temporal_stale_event_count = stats.stale_event_count,
        .temporal_retired_event_count = stats.retired_event_count,
        .temporal_active_event_count = stats.active_event_count,
        .temporal_unadmitted_process_count = stats.unadmitted_process_count,
        .temporal_evaluated_delta_ticks = stats.evaluated_delta_ticks,
        .temporal_catch_up_delta_ticks = stats.catch_up_delta_ticks,
        .temporal_maximum_lateness_ticks = stats.maximum_lateness_ticks,
        .temporal_oldest_deferred_lateness_ticks = stats.oldest_deferred_lateness_ticks,
        .temporal_event_budget_exhausted = stats.event_budget_exhausted,
        .temporal_catch_up_budget_exhausted = stats.catch_up_budget_exhausted,
        .temporal_counters_saturated = stats.counters_saturated,
    };
}

[[nodiscard]] std::uint64_t semantic_checksum(const ProcessInstance& process,
                                              std::uint64_t current) noexcept {
    current = combine_checksum(current, process.process_id.value());
    current = combine_checksum(current, process.owner_id.value());
    for (const auto character : process.prototype_id.value()) {
        current = combine_checksum(current, static_cast<unsigned char>(character));
    }
    current = combine_checksum(current, process.required_work_ticks);
    current = combine_checksum(current, process.accrued_work_ticks);
    current = combine_checksum(current, static_cast<std::uint64_t>(process.state));
    return combine_checksum(current, process.output_claimed ? 1U : 0U);
}

[[nodiscard]] core::Result<ProcessTemporalAggregationBenchmarkRepetition>
run_repetition(const ProcessTemporalAggregationBenchmarkConfig& config,
               const std::vector<ProcessInstance>& corpus, std::uint32_t repetition_index,
               bool retained, ProcessTemporalAggregationBenchmarkReport& report) {
    world::ProcessDatabase temporal_database;
    for (const auto& process : corpus) {
        auto status = temporal_database.insert(process);
        if (!status) {
            return core::Result<ProcessTemporalAggregationBenchmarkRepetition>::failure(
                status.error().code, status.error().message);
        }
    }
    auto dense = corpus;
    ProcessTemporalAggregationController controller(config.temporal);
    ProcessTemporalAggregationBenchmarkRepetition repetition;
    repetition.repetition = repetition_index;
    std::uint64_t temporal_resolver_calls = 0;
    std::uint64_t dense_resolver_calls = 0;
    const TemporalProcessModifierResolver resolver =
        [&config, &temporal_resolver_calls](const ProcessInstance& process) {
            ++temporal_resolver_calls;
            return core::Result<ProcessModifiers>::success(modifiers_for(config, process));
        };

    while (controller.admitted_process_count() < corpus.size()) {
        std::optional<core::Result<ProcessTemporalAggregationTickStats>> updated;
        const auto elapsed = measure_nanoseconds(
            [&] { updated.emplace(controller.update(temporal_database, 0, resolver)); });
        if (!updated->has_value()) {
            return core::Result<ProcessTemporalAggregationBenchmarkRepetition>::failure(
                updated->error().code, updated->error().message);
        }
        if (updated->value().admission_count == 0) {
            return core::Result<ProcessTemporalAggregationBenchmarkRepetition>::failure(
                "process_temporal_benchmark.admission_stalled",
                "temporal controller stopped admitting a non-empty process backlog");
        }
        ++repetition.admission_tick_count;
        repetition.admission_elapsed_nanoseconds += elapsed;
        if (updated->value().admission_count > config.temporal.maximum_admissions_per_tick ||
            updated->value().dispatched_event_count != 0) {
            ++repetition.budget_violation_count;
        }
        repetition.maximum_active_event_count =
            std::max(repetition.maximum_active_event_count, updated->value().active_event_count);
        repetition.maximum_unadmitted_process_count = std::max(
            repetition.maximum_unadmitted_process_count, updated->value().unadmitted_process_count);
    }

    std::uint32_t backlog_streak = 0;
    ProcessTemporalAggregationTickStats final_temporal_stats;
    for (simulation::WorldTick tick = 1; tick <= config.simulation_ticks; ++tick) {
        const auto temporal_calls_before = temporal_resolver_calls;
        const auto dense_calls_before = dense_resolver_calls;
        std::optional<core::Result<ProcessTemporalAggregationTickStats>> updated;
        auto temporal_status = core::Status::ok();
        auto dense_status = core::Status::ok();
        std::uint64_t temporal_elapsed = 0;
        std::uint64_t dense_elapsed = 0;
        const auto run_temporal = [&] {
            temporal_elapsed = measure_nanoseconds(
                [&] { updated.emplace(controller.update(temporal_database, tick, resolver)); });
            if (!updated.has_value()) {
                temporal_status =
                    core::Status::failure("process_temporal_benchmark.missing_temporal_result",
                                          "timed temporal update did not publish a result");
            } else if (!updated->has_value()) {
                temporal_status =
                    core::Status::failure(updated->error().code, updated->error().message);
            } else {
                final_temporal_stats = std::move(*updated).value();
            }
        };
        const auto run_dense = [&] {
            dense_elapsed = measure_nanoseconds([&] {
                for (auto& process : dense) {
                    ++dense_resolver_calls;
                    dense_status =
                        ProcessRuntime::advance(process, tick, modifiers_for(config, process));
                    if (!dense_status) {
                        break;
                    }
                }
            });
        };

        // Alternate the paired execution order across retained repetitions so neither path owns
        // the same cache/thermal position for the complete benchmark.
        if ((repetition_index & 1U) == 0U) {
            run_temporal();
            if (!temporal_status) {
                return core::Result<ProcessTemporalAggregationBenchmarkRepetition>::failure(
                    temporal_status.error().code, temporal_status.error().message);
            }
            run_dense();
        } else {
            run_dense();
            if (!dense_status) {
                return core::Result<ProcessTemporalAggregationBenchmarkRepetition>::failure(
                    dense_status.error().code, dense_status.error().message);
            }
            run_temporal();
        }
        if (!temporal_status) {
            return core::Result<ProcessTemporalAggregationBenchmarkRepetition>::failure(
                temporal_status.error().code, temporal_status.error().message);
        }
        if (!dense_status) {
            return core::Result<ProcessTemporalAggregationBenchmarkRepetition>::failure(
                dense_status.error().code, dense_status.error().message);
        }
        observe_temporal_stats(config, final_temporal_stats, repetition, backlog_streak, true);

        if (retained) {
            report.raw_samples.push_back(
                make_sample(repetition_index, tick, 0, temporal_elapsed, dense_elapsed,
                            temporal_resolver_calls - temporal_calls_before,
                            dense_resolver_calls - dense_calls_before, final_temporal_stats));
        }
    }

    const auto maximum_drain_passes =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(config.process_count) +
                                    config.temporal.maximum_events_per_tick - 1U) /
                                       config.temporal.maximum_events_per_tick +
                                   1U);
    std::uint32_t drain_pass = 0;
    while (final_temporal_stats.event_budget_exhausted) {
        if (++drain_pass > maximum_drain_passes) {
            return core::Result<ProcessTemporalAggregationBenchmarkRepetition>::failure(
                "process_temporal_benchmark.backlog_did_not_converge",
                "temporal event backlog did not converge at the final world tick");
        }
        const auto calls_before = temporal_resolver_calls;
        std::optional<core::Result<ProcessTemporalAggregationTickStats>> updated;
        const auto elapsed = measure_nanoseconds([&] {
            updated.emplace(
                controller.update(temporal_database, config.simulation_ticks, resolver));
        });
        if (!updated->has_value()) {
            return core::Result<ProcessTemporalAggregationBenchmarkRepetition>::failure(
                updated->error().code, updated->error().message);
        }
        final_temporal_stats = std::move(updated->value());
        // Artifact-only same-time drains make final semantic parity observable without pretending
        // that multiple controller calls are additional logical simulation ticks.
        observe_temporal_stats(config, final_temporal_stats, repetition, backlog_streak, false);
        if (retained) {
            report.raw_samples.push_back(
                make_sample(repetition_index, config.simulation_ticks, drain_pass, elapsed, 0,
                            temporal_resolver_calls - calls_before, 0, final_temporal_stats));
        }
    }

    repetition.temporal_resolver_call_count = temporal_resolver_calls;
    repetition.dense_resolver_call_count = dense_resolver_calls;
    for (std::size_t index = 0; index < corpus.size(); ++index) {
        const auto& expected = dense[index];
        repetition.dense_state_checksum =
            semantic_checksum(expected, repetition.dense_state_checksum);
        const auto* actual = temporal_database.find(expected.process_id);
        if (actual == nullptr) {
            ++repetition.parity_mismatch_count;
            continue;
        }
        repetition.temporal_state_checksum =
            semantic_checksum(*actual, repetition.temporal_state_checksum);
        if (actual->owner_id != expected.owner_id ||
            actual->prototype_id != expected.prototype_id ||
            actual->required_work_ticks != expected.required_work_ticks ||
            actual->accrued_work_ticks != expected.accrued_work_ticks ||
            actual->state != expected.state || actual->output_claimed != expected.output_claimed) {
            ++repetition.parity_mismatch_count;
        }
        repetition.timestamp_mismatch_count += actual->last_eval == expected.last_eval ? 0U : 1U;
        const auto stalled = is_stalled_process(config, expected.process_id);
        const auto expected_outcome =
            stalled ? actual->state == ProcessState::running && actual->accrued_work_ticks == 0
                    : actual->state == ProcessState::complete &&
                          actual->accrued_work_ticks == actual->required_work_ticks;
        repetition.unexpected_outcome_count += expected_outcome ? 0U : 1U;
    }
    return core::Result<ProcessTemporalAggregationBenchmarkRepetition>::success(
        std::move(repetition));
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
        default: {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20U) {
                constexpr std::string_view digits = "0123456789abcdef";
                result += "\\u00";
                result += digits[(byte >> 4U) & 0x0fU];
                result += digits[byte & 0x0fU];
            } else {
                result += character;
            }
            break;
        }
        }
    }
    return result;
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"' << json_escape(value) << '"';
}

void write_timing_summary(std::ostream& output,
                          const ProcessTemporalAggregationTimingSummary& summary,
                          std::string_view indentation) {
    output << indentation << "{\"sample_count\": " << summary.sample_count
           << ", \"minimum_ms\": " << summary.minimum_ms << ", \"median_ms\": " << summary.median_ms
           << ", \"p95_ms\": " << summary.p95_ms << ", \"p99_ms\": " << summary.p99_ms
           << ", \"maximum_ms\": " << summary.maximum_ms << ", \"mean_ms\": " << summary.mean_ms
           << ", \"standard_deviation_ms\": " << summary.standard_deviation_ms
           << ", \"coefficient_of_variation\": " << summary.coefficient_of_variation << '}';
}

[[nodiscard]] bool
valid_timing_summary(const ProcessTemporalAggregationTimingSummary& summary) noexcept {
    return summary.sample_count > 0 && std::isfinite(summary.minimum_ms) &&
           std::isfinite(summary.median_ms) && std::isfinite(summary.p95_ms) &&
           std::isfinite(summary.p99_ms) && std::isfinite(summary.maximum_ms) &&
           std::isfinite(summary.mean_ms) && std::isfinite(summary.standard_deviation_ms) &&
           std::isfinite(summary.coefficient_of_variation) && summary.minimum_ms >= 0.0 &&
           summary.minimum_ms <= summary.median_ms && summary.median_ms <= summary.p95_ms &&
           summary.p95_ms <= summary.p99_ms && summary.p99_ms <= summary.maximum_ms &&
           summary.standard_deviation_ms >= 0.0 && summary.coefficient_of_variation >= 0.0;
}

} // namespace

core::Status ProcessTemporalAggregationBenchmarkConfig::validate() const {
    if (process_count == 0 || process_count > maximum_process_count) {
        return core::Status::failure(
            "process_temporal_benchmark.invalid_process_count",
            "process temporal benchmark count must be between 1 and 1000000");
    }
    if (simulation_ticks == 0 || simulation_ticks > maximum_simulation_ticks) {
        return core::Status::failure(
            "process_temporal_benchmark.invalid_simulation_ticks",
            "process temporal benchmark duration must be between 1 and 1000000 ticks");
    }
    if (static_cast<std::uint64_t>(burst_process_count) + stalled_process_count > process_count) {
        return core::Status::failure(
            "process_temporal_benchmark.invalid_process_mix",
            "burst and stalled process counts cannot exceed the complete corpus");
    }
    if (stress_tick == 0 || stress_tick > simulation_ticks) {
        return core::Status::failure(
            "process_temporal_benchmark.invalid_stress_tick",
            "process temporal stress tick must fall within the simulated duration");
    }
    if (repetitions == 0 || repetitions > 100 || warmup_repetitions > 100 ||
        warmup_repetitions + repetitions > 100) {
        return core::Status::failure(
            "process_temporal_benchmark.invalid_repetitions",
            "process temporal benchmark requires 1..100 retained repetitions and at most 100 "
            "total passes");
    }
    auto temporal_status = temporal.validate();
    if (!temporal_status) {
        return temporal_status;
    }
    if (temporal.maximum_tracked_processes < process_count) {
        return core::Status::failure(
            "process_temporal_benchmark.insufficient_tracking_budget",
            "temporal tracking budget must cover the complete benchmark corpus");
    }
    if (!std::isfinite(maximum_temporal_p99_tick_ms) || maximum_temporal_p99_tick_ms < 0.0 ||
        !std::isfinite(minimum_median_speedup) || minimum_median_speedup < 0.0 ||
        !std::isfinite(minimum_resolver_call_reduction_ratio) ||
        minimum_resolver_call_reduction_ratio < 0.0 ||
        minimum_resolver_call_reduction_ratio > 1.0) {
        return core::Status::failure(
            "process_temporal_benchmark.invalid_acceptance_gate",
            "process temporal timing gates must be finite and non-negative, and resolver "
            "reduction must be within 0..1");
    }
    constexpr std::uint64_t maximum_raw_samples = 10'000'000;
    const auto maximum_drain_samples =
        (static_cast<std::uint64_t>(process_count) + temporal.maximum_events_per_tick - 1U) /
            temporal.maximum_events_per_tick +
        1U;
    const auto maximum_samples_per_repetition = simulation_ticks + maximum_drain_samples;
    if (maximum_samples_per_repetition > maximum_raw_samples / repetitions) {
        return core::Status::failure(
            "process_temporal_benchmark.excessive_sample_count",
            "process temporal benchmark raw sample count exceeds the safety limit");
    }
    constexpr std::uint64_t maximum_dense_process_evaluations = 2'000'000'000;
    const auto logical_tick_passes =
        simulation_ticks * static_cast<std::uint64_t>(warmup_repetitions + repetitions);
    if (process_count > maximum_dense_process_evaluations / logical_tick_passes) {
        return core::Status::failure(
            "process_temporal_benchmark.excessive_dense_work",
            "process temporal dense reference exceeds the bounded evaluation safety limit");
    }
    return core::Status::ok();
}

bool ProcessTemporalAggregationBenchmarkReport::acceptance_passed() const noexcept {
    return std::ranges::all_of(acceptance,
                               [](const auto& check) { return !check.enabled || check.passed; });
}

core::Status ProcessTemporalAggregationBenchmarkReport::validate() const {
    auto config_status = config.validate();
    if (!config_status) {
        return config_status;
    }
    if (repetitions.size() != config.repetitions) {
        return core::Status::failure(
            "process_temporal_benchmark.invalid_repetition_count",
            "process temporal report retained repetition count does not match its configuration");
    }
    const auto expected_dense_resolver_calls =
        static_cast<std::uint64_t>(config.process_count) * config.simulation_ticks;
    const auto expected_admission_ticks =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(config.process_count) +
                                    config.temporal.maximum_admissions_per_tick - 1U) /
                                   config.temporal.maximum_admissions_per_tick);
    for (std::size_t index = 0; index < repetitions.size(); ++index) {
        const auto& repetition = repetitions[index];
        if (repetition.repetition != index ||
            repetition.admission_tick_count != expected_admission_ticks ||
            repetition.admission_elapsed_nanoseconds == 0 ||
            repetition.temporal_resolver_call_count == 0 ||
            repetition.dense_resolver_call_count != expected_dense_resolver_calls ||
            repetition.maximum_event_lateness_ticks > config.simulation_ticks ||
            repetition.maximum_active_event_count > config.process_count ||
            repetition.maximum_unadmitted_process_count > config.process_count ||
            !valid_timing_summary(repetition.temporal_timing) ||
            !valid_timing_summary(repetition.dense_timing) ||
            repetition.temporal_timing.sample_count != config.simulation_ticks ||
            repetition.dense_timing.sample_count != config.simulation_ticks ||
            !std::isfinite(repetition.median_speedup) || repetition.median_speedup < 0.0 ||
            !std::isfinite(repetition.resolver_call_reduction_ratio) ||
            repetition.resolver_call_reduction_ratio < -1.0 ||
            repetition.resolver_call_reduction_ratio > 1.0) {
            return core::Status::failure(
                "process_temporal_benchmark.invalid_repetition",
                "process temporal report contains inconsistent repetition evidence");
        }
    }
    const auto expected_normal_samples =
        static_cast<std::uint64_t>(config.repetitions) * config.simulation_ticks;
    const auto normal_sample_count = static_cast<std::uint64_t>(std::ranges::count_if(
        raw_samples, [](const auto& sample) { return sample.drain_pass == 0; }));
    if (normal_sample_count != expected_normal_samples) {
        return core::Status::failure(
            "process_temporal_benchmark.invalid_sample_count",
            "process temporal report does not contain every retained logical tick");
    }
    std::vector<bool> normal_samples_seen(static_cast<std::size_t>(expected_normal_samples), false);
    std::vector<std::uint32_t> drain_sample_counts(config.repetitions, 0);
    for (const auto& sample : raw_samples) {
        const auto classified_events =
            static_cast<std::uint64_t>(sample.temporal_evaluated_process_count) +
            sample.temporal_stale_event_count + sample.temporal_retired_event_count;
        const auto expected_resolver_calls =
            static_cast<std::uint64_t>(sample.temporal_admission_count) +
            sample.temporal_dispatched_event_count - sample.temporal_retired_event_count;
        if (sample.repetition >= config.repetitions || sample.world_tick == 0 ||
            sample.world_tick > config.simulation_ticks ||
            sample.temporal_elapsed_nanoseconds == 0 ||
            (sample.drain_pass == 0 && sample.dense_elapsed_nanoseconds == 0) ||
            (sample.drain_pass != 0 && sample.dense_elapsed_nanoseconds != 0) ||
            sample.temporal_admission_count > config.temporal.maximum_admissions_per_tick ||
            sample.temporal_dispatched_event_count > config.temporal.maximum_events_per_tick ||
            sample.temporal_retired_event_count > sample.temporal_dispatched_event_count ||
            classified_events != sample.temporal_dispatched_event_count ||
            sample.temporal_resolver_call_count != expected_resolver_calls ||
            sample.temporal_changed_process_count > sample.temporal_evaluated_process_count ||
            sample.temporal_completed_process_count > sample.temporal_changed_process_count ||
            sample.temporal_active_event_count > config.process_count ||
            sample.temporal_unadmitted_process_count > config.process_count ||
            sample.temporal_catch_up_delta_ticks >
                config.temporal.maximum_catch_up_ticks_per_tick ||
            sample.temporal_maximum_lateness_ticks > sample.world_tick ||
            sample.temporal_oldest_deferred_lateness_ticks > sample.world_tick) {
            return core::Status::failure(
                "process_temporal_benchmark.invalid_sample",
                "process temporal report contains an invalid raw tick sample");
        }
        if (sample.drain_pass == 0) {
            const auto sample_index = static_cast<std::size_t>(sample.repetition) *
                                          static_cast<std::size_t>(config.simulation_ticks) +
                                      static_cast<std::size_t>(sample.world_tick - 1U);
            if (normal_samples_seen[sample_index] ||
                sample.dense_resolver_call_count != config.process_count ||
                sample.temporal_admission_count != 0) {
                return core::Status::failure(
                    "process_temporal_benchmark.invalid_logical_tick",
                    "process temporal report has a duplicate or inconsistent logical tick");
            }
            normal_samples_seen[sample_index] = true;
        } else {
            auto& drain_count = drain_sample_counts[sample.repetition];
            ++drain_count;
            if (sample.world_tick != config.simulation_ticks || sample.drain_pass != drain_count ||
                sample.dense_resolver_call_count != 0) {
                return core::Status::failure(
                    "process_temporal_benchmark.invalid_drain_sample",
                    "process temporal report contains a non-contiguous artifact drain");
            }
        }
    }
    if (!valid_timing_summary(temporal_timing) || !valid_timing_summary(dense_timing) ||
        temporal_timing.sample_count != expected_normal_samples ||
        dense_timing.sample_count != expected_normal_samples || !std::isfinite(median_speedup) ||
        median_speedup < 0.0 || !std::isfinite(minimum_resolver_call_reduction_ratio) ||
        minimum_resolver_call_reduction_ratio < -1.0 ||
        minimum_resolver_call_reduction_ratio > 1.0) {
        return core::Status::failure(
            "process_temporal_benchmark.invalid_summary",
            "process temporal report contains invalid timing or work-reduction summaries");
    }
    if (acceptance.empty()) {
        return core::Status::failure("process_temporal_benchmark.missing_acceptance",
                                     "process temporal report has no acceptance checks");
    }
    for (const auto& check : acceptance) {
        if (check.name.empty() || (check.comparison != "<=" && check.comparison != ">=") ||
            !std::isfinite(check.measured) || !std::isfinite(check.limit)) {
            return core::Status::failure(
                "process_temporal_benchmark.invalid_acceptance",
                "process temporal report contains an inconsistent acceptance check");
        }
        const auto comparison_passed = check.comparison == "<=" ? check.measured <= check.limit
                                                                : check.measured >= check.limit;
        const auto expected_passed = !check.enabled || comparison_passed;
        if (check.passed != expected_passed) {
            return core::Status::failure(
                "process_temporal_benchmark.invalid_acceptance",
                "process temporal report contains an inconsistent acceptance check");
        }
    }
    return core::Status::ok();
}

std::string ProcessTemporalAggregationBenchmarkReport::to_json() const {
    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{\n  \"schema_version\": " << schema_version
           << ",\n  \"benchmark\": \"process_temporal_aggregation\",\n  \"runtime\": {\n"
           << "    \"engine_version\": ";
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
           << "\n  },\n  \"config\": {\n"
           << "    \"process_count\": " << config.process_count
           << ",\n    \"simulation_ticks\": " << config.simulation_ticks
           << ",\n    \"burst_process_count\": " << config.burst_process_count
           << ",\n    \"stalled_process_count\": " << config.stalled_process_count
           << ",\n    \"stress_tick\": " << config.stress_tick << ",\n    \"seed\": " << config.seed
           << ",\n    \"warmup_repetitions\": " << config.warmup_repetitions
           << ",\n    \"repetitions\": " << config.repetitions
           << ",\n    \"maximum_admissions_per_tick\": "
           << config.temporal.maximum_admissions_per_tick
           << ",\n    \"maximum_events_per_tick\": " << config.temporal.maximum_events_per_tick
           << ",\n    \"maximum_tracked_processes\": " << config.temporal.maximum_tracked_processes
           << ",\n    \"stalled_reevaluation_interval_ticks\": "
           << config.temporal.stalled_reevaluation_interval_ticks
           << ",\n    \"maximum_catch_up_ticks_per_event\": "
           << config.temporal.maximum_catch_up_ticks_per_event
           << ",\n    \"maximum_catch_up_ticks_per_tick\": "
           << config.temporal.maximum_catch_up_ticks_per_tick
           << ",\n    \"maximum_event_backlog_ticks\": " << config.maximum_event_backlog_ticks
           << ",\n    \"maximum_temporal_p99_tick_ms\": " << config.maximum_temporal_p99_tick_ms
           << ",\n    \"minimum_median_speedup\": " << config.minimum_median_speedup
           << ",\n    \"minimum_resolver_call_reduction_ratio\": "
           << config.minimum_resolver_call_reduction_ratio << "\n  },\n  \"summary\": {\n"
           << "    \"temporal_timing\": ";
    write_timing_summary(output, temporal_timing, "");
    output << ",\n    \"dense_timing\": ";
    write_timing_summary(output, dense_timing, "");
    output << ",\n    \"median_speedup\": " << median_speedup
           << ",\n    \"minimum_resolver_call_reduction_ratio\": "
           << minimum_resolver_call_reduction_ratio
           << ",\n    \"acceptance_passed\": " << (acceptance_passed() ? "true" : "false")
           << "\n  },\n  \"acceptance\": [\n";
    for (std::size_t index = 0; index < acceptance.size(); ++index) {
        const auto& check = acceptance[index];
        output << "    {\"name\": ";
        write_json_string(output, check.name);
        output << ", \"comparison\": ";
        write_json_string(output, check.comparison);
        output << ", \"measured\": " << check.measured << ", \"limit\": " << check.limit
               << ", \"enabled\": " << (check.enabled ? "true" : "false")
               << ", \"passed\": " << (check.passed ? "true" : "false") << '}'
               << (index + 1U == acceptance.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"repetitions\": [\n";
    for (std::size_t index = 0; index < repetitions.size(); ++index) {
        const auto& repetition = repetitions[index];
        output << "    {\"repetition\": " << repetition.repetition
               << ", \"admission_tick_count\": " << repetition.admission_tick_count
               << ", \"admission_elapsed_nanoseconds\": "
               << repetition.admission_elapsed_nanoseconds << ", \"temporal_timing\": ";
        write_timing_summary(output, repetition.temporal_timing, "");
        output << ", \"dense_timing\": ";
        write_timing_summary(output, repetition.dense_timing, "");
        output << ", \"median_speedup\": " << repetition.median_speedup
               << ", \"resolver_call_reduction_ratio\": "
               << repetition.resolver_call_reduction_ratio
               << ", \"temporal_resolver_call_count\": " << repetition.temporal_resolver_call_count
               << ", \"dense_resolver_call_count\": " << repetition.dense_resolver_call_count
               << ", \"maximum_event_backlog_ticks\": " << repetition.maximum_event_backlog_ticks
               << ", \"maximum_event_lateness_ticks\": " << repetition.maximum_event_lateness_ticks
               << ", \"maximum_active_event_count\": " << repetition.maximum_active_event_count
               << ", \"maximum_unadmitted_process_count\": "
               << repetition.maximum_unadmitted_process_count
               << ", \"maximum_deferred_lateness_ticks\": "
               << repetition.maximum_deferred_lateness_ticks
               << ", \"budget_violation_count\": " << repetition.budget_violation_count
               << ", \"parity_mismatch_count\": " << repetition.parity_mismatch_count
               << ", \"timestamp_mismatch_count\": " << repetition.timestamp_mismatch_count
               << ", \"unexpected_outcome_count\": " << repetition.unexpected_outcome_count
               << ", \"temporal_state_checksum\": " << repetition.temporal_state_checksum
               << ", \"dense_state_checksum\": " << repetition.dense_state_checksum << '}'
               << (index + 1U == repetitions.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"raw_samples\": [\n";
    for (std::size_t index = 0; index < raw_samples.size(); ++index) {
        const auto& sample = raw_samples[index];
        output
            << "    {\"repetition\": " << sample.repetition
            << ", \"world_tick\": " << sample.world_tick
            << ", \"drain_pass\": " << sample.drain_pass
            << ", \"temporal_elapsed_nanoseconds\": " << sample.temporal_elapsed_nanoseconds
            << ", \"dense_elapsed_nanoseconds\": " << sample.dense_elapsed_nanoseconds
            << ", \"temporal_resolver_call_count\": " << sample.temporal_resolver_call_count
            << ", \"dense_resolver_call_count\": " << sample.dense_resolver_call_count
            << ", \"temporal_admission_count\": " << sample.temporal_admission_count
            << ", \"temporal_dispatched_event_count\": " << sample.temporal_dispatched_event_count
            << ", \"temporal_evaluated_process_count\": " << sample.temporal_evaluated_process_count
            << ", \"temporal_changed_process_count\": " << sample.temporal_changed_process_count
            << ", \"temporal_completed_process_count\": " << sample.temporal_completed_process_count
            << ", \"temporal_stale_event_count\": " << sample.temporal_stale_event_count
            << ", \"temporal_retired_event_count\": " << sample.temporal_retired_event_count
            << ", \"temporal_active_event_count\": " << sample.temporal_active_event_count
            << ", \"temporal_unadmitted_process_count\": "
            << sample.temporal_unadmitted_process_count
            << ", \"temporal_evaluated_delta_ticks\": " << sample.temporal_evaluated_delta_ticks
            << ", \"temporal_catch_up_delta_ticks\": " << sample.temporal_catch_up_delta_ticks
            << ", \"temporal_maximum_lateness_ticks\": " << sample.temporal_maximum_lateness_ticks
            << ", \"temporal_oldest_deferred_lateness_ticks\": "
            << sample.temporal_oldest_deferred_lateness_ticks
            << ", \"temporal_event_budget_exhausted\": "
            << (sample.temporal_event_budget_exhausted ? "true" : "false")
            << ", \"temporal_catch_up_budget_exhausted\": "
            << (sample.temporal_catch_up_budget_exhausted ? "true" : "false")
            << ", \"temporal_counters_saturated\": "
            << (sample.temporal_counters_saturated ? "true" : "false") << '}'
            << (index + 1U == raw_samples.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

core::Status
ProcessTemporalAggregationBenchmarkReport::write_json(const std::filesystem::path& path) const {
    if (path.empty()) {
        return core::Status::failure("process_temporal_benchmark.empty_output_path",
                                     "process temporal benchmark output path is empty");
    }
    auto report_status = validate();
    if (!report_status) {
        return report_status;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Status::failure("process_temporal_benchmark.open_failed",
                                     "failed to open process temporal benchmark output");
    }
    const auto json = to_json();
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!output) {
        return core::Status::failure("process_temporal_benchmark.write_failed",
                                     "failed to write process temporal benchmark output");
    }
    return core::Status::ok();
}

core::Result<ProcessTemporalAggregationBenchmarkReport> run_process_temporal_aggregation_benchmark(
    const ProcessTemporalAggregationBenchmarkConfig& config) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.process_temporal_aggregation");
    auto config_status = config.validate();
    if (!config_status) {
        return core::Result<ProcessTemporalAggregationBenchmarkReport>::failure(
            config_status.error().code, config_status.error().message);
    }
    auto corpus = build_corpus(config);
    if (!corpus) {
        return core::Result<ProcessTemporalAggregationBenchmarkReport>::failure(
            corpus.error().code, corpus.error().message);
    }

    ProcessTemporalAggregationBenchmarkReport report;
    report.config = config;
    report.runtime = profiling::query_runtime_metadata();
    report.raw_samples.reserve(static_cast<std::size_t>(config.repetitions) *
                               static_cast<std::size_t>(config.simulation_ticks));
    report.repetitions.reserve(config.repetitions);
    const auto total_passes = config.warmup_repetitions + config.repetitions;
    for (std::uint32_t pass = 0; pass < total_passes; ++pass) {
        const auto retained = pass >= config.warmup_repetitions;
        const auto repetition_index = retained ? pass - config.warmup_repetitions : 0;
        auto repetition =
            run_repetition(config, corpus.value(), repetition_index, retained, report);
        if (!repetition) {
            return core::Result<ProcessTemporalAggregationBenchmarkReport>::failure(
                repetition.error().code, repetition.error().message);
        }
        if (retained) {
            report.repetitions.push_back(std::move(repetition).value());
        }
    }

    std::vector<std::uint64_t> temporal_timings;
    std::vector<std::uint64_t> dense_timings;
    std::vector<std::vector<std::uint64_t>> repetition_temporal_timings(config.repetitions);
    std::vector<std::vector<std::uint64_t>> repetition_dense_timings(config.repetitions);
    temporal_timings.reserve(static_cast<std::size_t>(config.repetitions) *
                             static_cast<std::size_t>(config.simulation_ticks));
    dense_timings.reserve(temporal_timings.capacity());
    for (std::uint32_t index = 0; index < config.repetitions; ++index) {
        repetition_temporal_timings[index].reserve(
            static_cast<std::size_t>(config.simulation_ticks));
        repetition_dense_timings[index].reserve(static_cast<std::size_t>(config.simulation_ticks));
    }
    for (const auto& sample : report.raw_samples) {
        if (sample.drain_pass != 0) {
            continue;
        }
        temporal_timings.push_back(sample.temporal_elapsed_nanoseconds);
        dense_timings.push_back(sample.dense_elapsed_nanoseconds);
        repetition_temporal_timings[sample.repetition].push_back(
            sample.temporal_elapsed_nanoseconds);
        repetition_dense_timings[sample.repetition].push_back(sample.dense_elapsed_nanoseconds);
    }
    report.temporal_timing = summarize_nanoseconds(std::move(temporal_timings));
    report.dense_timing = summarize_nanoseconds(std::move(dense_timings));
    std::vector<double> repetition_speedups;
    repetition_speedups.reserve(config.repetitions);
    for (std::size_t index = 0; index < report.repetitions.size(); ++index) {
        auto& repetition = report.repetitions[index];
        repetition.temporal_timing =
            summarize_nanoseconds(std::move(repetition_temporal_timings[index]));
        repetition.dense_timing = summarize_nanoseconds(std::move(repetition_dense_timings[index]));
        repetition.median_speedup =
            repetition.temporal_timing.median_ms == 0.0
                ? 0.0
                : repetition.dense_timing.median_ms / repetition.temporal_timing.median_ms;
        repetition.resolver_call_reduction_ratio =
            repetition.dense_resolver_call_count == 0
                ? 0.0
                : 1.0 - static_cast<double>(repetition.temporal_resolver_call_count) /
                            static_cast<double>(repetition.dense_resolver_call_count);
        repetition_speedups.push_back(repetition.median_speedup);
    }
    std::ranges::sort(repetition_speedups);
    report.median_speedup = percentile(repetition_speedups, 0.50);

    std::uint64_t total_parity_mismatches = 0;
    std::uint64_t total_unexpected_outcomes = 0;
    std::uint64_t total_budget_violations = 0;
    std::uint32_t maximum_backlog_ticks = 0;
    simulation::WorldTick maximum_event_lateness_ticks = 0;
    std::uint64_t checksum_mismatches = 0;
    report.minimum_resolver_call_reduction_ratio = 1.0;
    const auto first_checksum = report.repetitions.front().temporal_state_checksum;
    for (const auto& repetition : report.repetitions) {
        total_parity_mismatches += repetition.parity_mismatch_count;
        total_unexpected_outcomes += repetition.unexpected_outcome_count;
        total_budget_violations += repetition.budget_violation_count;
        maximum_backlog_ticks =
            std::max(maximum_backlog_ticks, repetition.maximum_event_backlog_ticks);
        maximum_event_lateness_ticks =
            std::max(maximum_event_lateness_ticks, repetition.maximum_event_lateness_ticks);
        checksum_mismatches +=
            repetition.temporal_state_checksum == repetition.dense_state_checksum &&
                    repetition.temporal_state_checksum == first_checksum
                ? 0U
                : 1U;
        report.minimum_resolver_call_reduction_ratio = std::min(
            report.minimum_resolver_call_reduction_ratio, repetition.resolver_call_reduction_ratio);
    }

    const auto add_maximum_check = [&report](std::string name, double measured, double limit,
                                             bool enabled = true) {
        report.acceptance.push_back(
            {std::move(name), "<=", measured, limit, enabled, !enabled || measured <= limit});
    };
    const auto add_minimum_check = [&report](std::string name, double measured, double limit,
                                             bool enabled = true) {
        report.acceptance.push_back(
            {std::move(name), ">=", measured, limit, enabled, !enabled || measured >= limit});
    };
    add_maximum_check("dense_reference_semantic_parity_mismatches",
                      static_cast<double>(total_parity_mismatches), 0.0);
    add_maximum_check("deterministic_checksum_mismatches", static_cast<double>(checksum_mismatches),
                      0.0);
    add_maximum_check("unexpected_process_outcomes", static_cast<double>(total_unexpected_outcomes),
                      0.0);
    add_maximum_check("hard_budget_violations", static_cast<double>(total_budget_violations), 0.0);
    add_maximum_check("maximum_event_backlog_ticks", static_cast<double>(maximum_backlog_ticks),
                      static_cast<double>(config.maximum_event_backlog_ticks));
    add_maximum_check("maximum_event_lateness_ticks",
                      static_cast<double>(maximum_event_lateness_ticks),
                      static_cast<double>(config.maximum_event_backlog_ticks));
    add_maximum_check("temporal_p99_tick_ms", report.temporal_timing.p99_ms,
                      config.maximum_temporal_p99_tick_ms,
                      config.maximum_temporal_p99_tick_ms > 0.0);
    add_minimum_check("median_speedup", report.median_speedup, config.minimum_median_speedup,
                      config.minimum_median_speedup > 0.0);
    add_minimum_check("resolver_call_reduction_ratio", report.minimum_resolver_call_reduction_ratio,
                      config.minimum_resolver_call_reduction_ratio,
                      config.minimum_resolver_call_reduction_ratio > 0.0);

    auto report_status = report.validate();
    if (!report_status) {
        return core::Result<ProcessTemporalAggregationBenchmarkReport>::failure(
            report_status.error().code, report_status.error().message);
    }
    return core::Result<ProcessTemporalAggregationBenchmarkReport>::success(std::move(report));
}

} // namespace heartstead::processes::benchmark
