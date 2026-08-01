#include "game/application/runtime_diagnostics.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

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

} // namespace

void FrameRateCounter::record_frame(std::uint64_t delta_microseconds) noexcept {
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
    accumulated_microseconds_ = 0;
    accumulated_frames_ = 0;
}

void FrameRateCounter::reset() noexcept {
    accumulated_microseconds_ = 0;
    accumulated_frames_ = 0;
    sample_ = {};
}

FrameRateSample FrameRateCounter::sample() const noexcept {
    return sample_;
}

ProcessResourceSample sample_process_resources() noexcept {
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
    sample.thread_count = directory_entry_count("/proc/self/task");
    sample.open_file_count = directory_entry_count("/proc/self/fd");
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
        output << bytes_text(*snapshot.process.resident_memory_bytes);
    else
        output << "unavailable";
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

} // namespace heartstead::game
