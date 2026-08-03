#include "game/application/runtime_diagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace heartstead::game {

namespace {

[[nodiscard]] std::optional<std::size_t> directory_entry_count(const char* path) noexcept {
    std::error_code error;
    std::filesystem::directory_iterator iterator(path, error);
    if (error) {
        return std::nullopt;
    }
    std::size_t count = 0;
    for (const auto& entry : iterator) {
        (void)entry;
        ++count;
    }
    return count;
}

[[nodiscard]] std::string bytes_text(std::uint64_t bytes) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(1)
           << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MiB";
    return output.str();
}

#if defined(__linux__)
[[nodiscard]] std::optional<std::uint64_t> kib_field_value(std::string_view line,
                                                           std::string_view expected_name) {
    if (!line.starts_with(expected_name)) {
        return std::nullopt;
    }
    std::istringstream fields{std::string(line.substr(expected_name.size()))};
    std::uint64_t kibibytes = 0;
    std::string unit;
    if (!(fields >> kibibytes >> unit) || unit != "kB" ||
        kibibytes > std::numeric_limits<std::uint64_t>::max() / 1024U) {
        return std::nullopt;
    }
    return kibibytes * 1024U;
}

void sample_precise_linux_memory(ProcessResourceSample& sample) noexcept {
    std::ifstream rollup("/proc/self/smaps_rollup");
    if (!rollup) {
        return;
    }
    std::optional<std::uint64_t> precise_rss;
    std::optional<std::uint64_t> proportional_set_size;
    std::optional<std::uint64_t> private_clean;
    std::optional<std::uint64_t> private_dirty;
    std::string line;
    while (std::getline(rollup, line)) {
        if (auto rss = kib_field_value(line, "Rss:")) {
            precise_rss = *rss;
        } else if (auto pss = kib_field_value(line, "Pss:")) {
            proportional_set_size = *pss;
        } else if (auto clean = kib_field_value(line, "Private_Clean:")) {
            private_clean = *clean;
        } else if (auto dirty = kib_field_value(line, "Private_Dirty:")) {
            private_dirty = *dirty;
        }
    }
    if (!precise_rss.has_value() || !proportional_set_size.has_value() ||
        !private_clean.has_value() || !private_dirty.has_value() ||
        *private_clean > std::numeric_limits<std::uint64_t>::max() - *private_dirty) {
        return;
    }
    sample.resident_memory_bytes = *precise_rss;
    sample.proportional_set_size_bytes = *proportional_set_size;
    sample.private_resident_memory_bytes = *private_clean + *private_dirty;
    sample.precise_memory_accounting = true;
}
#endif

} // namespace

void FrameRateCounter::record_frame(std::uint64_t delta_microseconds) noexcept {
    const auto process_cpu_clock = std::clock();
    accumulated_microseconds_ += delta_microseconds;
    ++accumulated_frames_;
    if (accumulated_microseconds_ < refresh_interval_microseconds &&
        sample_.frames_per_second > 0.0) {
        return;
    }
    const auto elapsed = static_cast<double>(accumulated_microseconds_);
    const auto frames = static_cast<double>(accumulated_frames_);
    if (elapsed > 0.0 && frames > 0.0) {
        sample_.frames_per_second = frames * 1'000'000.0 / elapsed;
        sample_.frame_time_milliseconds = elapsed / frames / 1'000.0;
    }
    if (elapsed > 0.0 && last_process_cpu_clock_.has_value() &&
        process_cpu_clock != static_cast<std::clock_t>(-1) &&
        process_cpu_clock >= *last_process_cpu_clock_) {
        const auto logical_processor_count = std::max(1U, std::thread::hardware_concurrency());
        const auto process_seconds =
            static_cast<double>(process_cpu_clock - *last_process_cpu_clock_) /
            static_cast<double>(CLOCKS_PER_SEC);
        const auto wall_seconds = elapsed / 1'000'000.0;
        sample_.process_cpu_usage_percent = std::clamp(
            process_seconds / wall_seconds / static_cast<double>(logical_processor_count) * 100.0,
            0.0, 100.0);
    }
    if (process_cpu_clock != static_cast<std::clock_t>(-1)) {
        last_process_cpu_clock_ = process_cpu_clock;
    }
    accumulated_microseconds_ = 0;
    accumulated_frames_ = 0;
}

void FrameRateCounter::reset() noexcept {
    accumulated_microseconds_ = 0;
    accumulated_frames_ = 0;
    sample_ = {};
    last_process_cpu_clock_.reset();
}

FrameRateSample FrameRateCounter::sample() const noexcept {
    return sample_;
}

ProcessResourceSample sample_process_resources(ProcessMemoryDetail memory_detail) noexcept {
    ProcessResourceSample sample;
#if defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    std::uint64_t virtual_pages = 0;
    std::uint64_t resident_pages = 0;
    if (statm >> virtual_pages >> resident_pages) {
        (void)virtual_pages;
        const auto page_size = ::sysconf(_SC_PAGESIZE);
        if (page_size > 0) {
            sample.resident_memory_bytes =
                resident_pages * static_cast<std::uint64_t>(page_size);
        }
    }
    if (memory_detail == ProcessMemoryDetail::precise) {
        sample_precise_linux_memory(sample);
    }
    sample.thread_count = directory_entry_count("/proc/self/task");
    sample.open_file_count = directory_entry_count("/proc/self/fd");
#else
    (void)memory_detail;
#endif
    return sample;
}

std::string format_runtime_diagnostics(const RuntimeDiagnosticsSnapshot& snapshot) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << "RUNTIME DIAGNOSTICS [F3]\n"
           << "application " << application_state_name(snapshot.application_state)
           << " | loading "
           << (snapshot.loading_phase.has_value()
                   ? session_startup_phase_name(*snapshot.loading_phase)
                   : "none")
           << '\n'
           << "session "
           << (snapshot.session_state.has_value()
                   ? runtime_session_state_name(*snapshot.session_state)
                   : "none")
           << " | mode "
           << (snapshot.session_mode.has_value() ? session_mode_name(*snapshot.session_mode)
                                                 : "none")
           << " | connection "
           << (snapshot.connection_state.has_value()
                   ? session_connection_state_name(*snapshot.connection_state)
                   : "none")
           << " | generation " << snapshot.session_generation << '\n'
           << "world " << (snapshot.active_world.empty() ? "none" : snapshot.active_world)
           << " | save "
           << (snapshot.save_destination.empty() ? "ephemeral" : snapshot.save_destination) << '\n'
           << "ticks authoritative/fixed " << snapshot.authoritative_tick << '/'
           << snapshot.fixed_step_tick << " | interpolation " << snapshot.interpolation_alpha
           << " | dropped " << snapshot.dropped_tick_time_us << " us\n"
           << "jobs/loading/network " << snapshot.active_jobs << '/'
           << snapshot.pending_loading_operations << '/' << snapshot.active_network_connections
           << " | callbacks " << snapshot.registered_session_callbacks << '\n'
           << "chunk load pending/reserved " << snapshot.pending_chunk_load_operations << '/'
           << bytes_text(snapshot.reserved_chunk_load_working_bytes) << " | worker/pipeline "
           << snapshot.last_chunk_load_worker_ms << '/'
           << snapshot.maximum_chunk_load_pipeline_latency_ms << " ms | publish max "
           << snapshot.maximum_chunk_load_publication_us << " us\n"
           << "stream " << (snapshot.predictive_streaming_enabled ? "predictive" : "inactive")
           << " desired/speculative/deferred/overage " << snapshot.desired_streaming_chunks << '/'
           << snapshot.active_speculative_streaming_chunks << '/'
           << snapshot.deferred_streaming_evictions << '/'
           << snapshot.projected_streaming_overage << " | accuracy/coverage "
           << snapshot.streaming_prediction_accuracy << '/'
           << snapshot.streaming_timely_coverage << '\n'
           << "save pending/reserved " << snapshot.pending_save_operations << '/'
           << bytes_text(snapshot.reserved_save_working_bytes) << " | owner last/max "
           << snapshot.last_save_owner_handoff_ms << '/' << snapshot.maximum_save_owner_handoff_ms
           << " ms | durable/worker " << snapshot.last_save_durable_acceptance_ms << '/'
           << snapshot.last_save_worker_ms << " ms\n"
           << "checkpoint pending/inflight/retries/completed/exhausted/terminal "
           << snapshot.pending_save_checkpoints << '/' << snapshot.in_flight_save_checkpoints << '/'
           << snapshot.save_checkpoint_retry_attempts << '/'
           << snapshot.completed_save_checkpoints << '/'
           << snapshot.exhausted_save_checkpoints << '/'
           << snapshot.terminal_save_checkpoint_failures << '\n'
           << "entities/physics/presentation/render " << snapshot.world_entities << '/'
           << snapshot.physics_objects << '/' << snapshot.presentation_objects << '/'
           << snapshot.render_objects << " | audio " << snapshot.audio_emitters << " | assets "
           << snapshot.asset_references << '\n'
           << "GPU resident " << bytes_text(snapshot.resident_gpu_bytes) << " | device ";
    if (snapshot.device_gpu_usage_bytes.has_value() &&
        snapshot.device_gpu_budget_bytes.has_value()) {
        output << bytes_text(*snapshot.device_gpu_usage_bytes) << " / "
               << bytes_text(*snapshot.device_gpu_budget_bytes);
    } else {
        output << "budget telemetry unavailable";
    }
    output << "\nprocess RSS ";
    if (snapshot.process.resident_memory_bytes.has_value())
        output << bytes_text(*snapshot.process.resident_memory_bytes)
               << (snapshot.process.precise_memory_accounting ? " (smaps)" : " (statm)");
    else
        output << "unavailable";
    if (snapshot.process.private_resident_memory_bytes.has_value() ||
        snapshot.process.proportional_set_size_bytes.has_value()) {
        output << " | private ";
        if (snapshot.process.private_resident_memory_bytes.has_value())
            output << bytes_text(*snapshot.process.private_resident_memory_bytes);
        else
            output << "unavailable";
        output << " | PSS ";
        if (snapshot.process.proportional_set_size_bytes.has_value())
            output << bytes_text(*snapshot.process.proportional_set_size_bytes);
        else
            output << "unavailable";
    }
    output << " | threads ";
    if (snapshot.process.thread_count.has_value())
        output << *snapshot.process.thread_count;
    else
        output << "unavailable";
    output << " | open files ";
    if (snapshot.process.open_file_count.has_value())
        output << *snapshot.process.open_file_count;
    else
        output << "unavailable";
    return output.str();
}

std::string format_frame_rate(FrameRateSample sample) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << "FPS " << sample.frames_per_second;
    return output.str();
}

std::string format_performance_overlay(const PerformanceOverlaySnapshot& snapshot) {
    const auto frame_interval_ms = snapshot.frame_rate.frame_time_milliseconds;
    const auto gpu_load_percent =
        snapshot.gpu_timing_valid && std::isfinite(snapshot.gpu_frame_milliseconds) &&
                snapshot.gpu_frame_milliseconds >= 0.0 && std::isfinite(frame_interval_ms) &&
                frame_interval_ms > 0.0
            ? std::optional<double>{std::clamp(
                  snapshot.gpu_frame_milliseconds / frame_interval_ms * 100.0, 0.0, 100.0)}
            : std::nullopt;

    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << "FPS "
           << std::max(0.0, snapshot.frame_rate.frames_per_second) << '\n'
           << std::setprecision(2) << "CPU FRAME "
           << std::max(0.0, snapshot.cpu_frame_milliseconds) << " MS\nCPU USE ";
    if (snapshot.frame_rate.process_cpu_usage_percent.has_value() &&
        std::isfinite(*snapshot.frame_rate.process_cpu_usage_percent)) {
        output << std::setprecision(1)
               << std::clamp(*snapshot.frame_rate.process_cpu_usage_percent, 0.0, 100.0) << " %";
    } else {
        output << "N/A";
    }
    output << "\nGPU FRAME ";
    if (snapshot.gpu_timing_valid && std::isfinite(snapshot.gpu_frame_milliseconds) &&
        snapshot.gpu_frame_milliseconds >= 0.0) {
        output << std::setprecision(2) << snapshot.gpu_frame_milliseconds << " MS";
    } else {
        output << "N/A";
    }
    output << "\nGPU LOAD ";
    if (gpu_load_percent.has_value()) {
        output << std::setprecision(1) << *gpu_load_percent << " %";
    } else {
        output << "N/A";
    }
    output << "\nWORLD RAM ";
    if (snapshot.process_resident_memory_bytes.has_value()) {
        output << bytes_text(*snapshot.process_resident_memory_bytes);
    } else {
        output << "N/A";
    }
    output << "\nGPU MESH " << bytes_text(snapshot.gpu_mesh_memory_bytes) << "\nCHUNKS "
           << snapshot.resident_chunks << "\nVISIBLE " << snapshot.visible_chunks
           << "\nOCCLUDED " << snapshot.occluded_chunks;
    return output.str();
}

} // namespace heartstead::game
