#include "game/application/application_benchmark.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <ranges>
#include <sstream>
#include <string_view>

namespace heartstead::game {

namespace {

[[nodiscard]] double percentile(const std::vector<double>& sorted, double fraction) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto position = std::clamp(fraction, 0.0, 1.0) *
                          static_cast<double>(sorted.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const auto weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

[[nodiscard]] double mean(const std::vector<double>& values) noexcept {
    return values.empty()
               ? 0.0
               : std::accumulate(values.begin(), values.end(), 0.0) /
                     static_cast<double>(values.size());
}

[[nodiscard]] double fps_from_frame_time(double milliseconds) noexcept {
    return milliseconds > 0.0 ? 1'000.0 / milliseconds : 0.0;
}

[[nodiscard]] std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
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

} // namespace

core::Status GameApplicationBenchmarkConfig::validate() const {
    if (measured_frames == 0) {
        return core::Status::failure("game_application.invalid_benchmark_frame_count",
                                     "benchmark measured frame count must be non-zero");
    }
    if (maximum_startup_frames == 0) {
        return core::Status::failure("game_application.invalid_benchmark_startup_limit",
                                     "benchmark startup frame limit must be non-zero");
    }
    return core::Status::ok();
}

GameApplicationBenchmarkSummary
summarize_application_benchmark(const GameApplicationBenchmarkReport& report) {
    GameApplicationBenchmarkSummary summary;
    summary.sample_count = report.samples.size();
    if (report.samples.empty()) {
        return summary;
    }

    std::vector<double> frames;
    std::vector<double> updates;
    std::vector<double> renders;
    frames.reserve(report.samples.size());
    updates.reserve(report.samples.size());
    renders.reserve(report.samples.size());
    double event_total = 0.0;
    double state_update_total = 0.0;
    double camera_world_total = 0.0;
    double audio_total = 0.0;
    double ui_total = 0.0;
    double renderer_cpu_total = 0.0;
    double gpu_total = 0.0;
    double chunk_sync_total = 0.0;
    double extraction_total = 0.0;
    double culling_total = 0.0;
    double draw_list_total = 0.0;
    double command_build_total = 0.0;
    double command_recording_total = 0.0;
    double frontend_preparation_total = 0.0;
    double lighting_preparation_total = 0.0;
    double shadow_preparation_total = 0.0;
    double chunk_draw_preparation_total = 0.0;
    double scene_preparation_total = 0.0;
    double shadow_command_build_total = 0.0;
    double debug_ui_preparation_total = 0.0;
    double backend_execute_total = 0.0;
    double backend_frame_setup_total = 0.0;
    double queue_submit_total = 0.0;
    double gpu_wait_total = 0.0;
    double frame_context_wait_total = 0.0;
    double swapchain_acquire_total = 0.0;
    double queue_present_total = 0.0;
    double gpu_shadow_total = 0.0;
    double gpu_sky_total = 0.0;
    double gpu_opaque_total = 0.0;
    double gpu_alpha_tested_total = 0.0;
    double gpu_transparent_total = 0.0;
    double gpu_tone_map_total = 0.0;
    double gpu_ui_total = 0.0;
    double gpu_transfer_total = 0.0;
    double gpu_final_copy_total = 0.0;
    double slowest = -1.0;
    std::size_t renderer_samples = 0;

    for (const auto& sample : report.samples) {
        frames.push_back(sample.total_frame_ms);
        updates.push_back(sample.mode_update_ms);
        renders.push_back(sample.render_ms);
        event_total += sample.event_pump_ms;
        state_update_total += sample.mode.state_update_ms;
        camera_world_total += sample.mode.camera_world_ms;
        audio_total += sample.mode.audio_ms;
        ui_total += sample.mode.ui_ms;
        if (sample.total_frame_ms > slowest) {
            slowest = sample.total_frame_ms;
            summary.slowest_frame_index = sample.frame_index;
        }
        if (!sample.renderer.has_value()) {
            continue;
        }
        ++renderer_samples;
        const auto& renderer = *sample.renderer;
        renderer_cpu_total += renderer.cpu_frame_ms;
        chunk_sync_total += renderer.chunk_synchronization_ms;
        extraction_total += renderer.render_extraction_ms;
        culling_total += renderer.culling_ms;
        draw_list_total += renderer.draw_list_ms;
        command_build_total += renderer.command_build_ms;
        command_recording_total += renderer.command_recording_ms;
        frontend_preparation_total += renderer.frontend_preparation_ms;
        lighting_preparation_total += renderer.lighting_preparation_ms;
        shadow_preparation_total += renderer.shadow_preparation_ms;
        chunk_draw_preparation_total += renderer.chunk_draw_preparation_ms;
        scene_preparation_total += renderer.scene_preparation_ms;
        shadow_command_build_total += renderer.shadow_command_build_ms;
        debug_ui_preparation_total += renderer.debug_ui_preparation_ms;
        backend_execute_total += renderer.backend_execute_ms;
        backend_frame_setup_total += renderer.backend_frame_setup_ms;
        queue_submit_total += renderer.queue_submit_ms;
        gpu_wait_total += renderer.gpu_wait_ms;
        frame_context_wait_total += renderer.frame_context_wait_ms;
        swapchain_acquire_total += renderer.swapchain_acquire_ms;
        queue_present_total += renderer.queue_present_ms;
        if (renderer.gpu_timing_valid) {
            gpu_total += renderer.gpu_frame_ms;
            gpu_shadow_total += renderer.gpu_shadow_ms;
            gpu_sky_total += renderer.gpu_sky_ms;
            gpu_opaque_total += renderer.gpu_opaque_terrain_ms;
            gpu_alpha_tested_total += renderer.gpu_alpha_tested_terrain_ms;
            gpu_transparent_total += renderer.gpu_transparent_terrain_ms;
            gpu_tone_map_total += renderer.gpu_tone_map_ms;
            gpu_ui_total += renderer.gpu_ui_ms;
            gpu_transfer_total += renderer.gpu_transfer_ms;
            gpu_final_copy_total += renderer.gpu_final_copy_ms;
            ++summary.gpu_sample_count;
        }
    }

    std::ranges::sort(frames);
    std::ranges::sort(updates);
    std::ranges::sort(renders);
    summary.median_frame_ms = percentile(frames, 0.50);
    summary.p95_frame_ms = percentile(frames, 0.95);
    summary.p99_frame_ms = percentile(frames, 0.99);
    summary.maximum_frame_ms = frames.back();
    summary.one_percent_low_fps = fps_from_frame_time(summary.p99_frame_ms);
    summary.point_one_percent_low_fps = fps_from_frame_time(percentile(frames, 0.999));
    summary.mean_event_pump_ms = event_total / static_cast<double>(report.samples.size());
    summary.mean_mode_update_ms = mean(updates);
    summary.p95_mode_update_ms = percentile(updates, 0.95);
    summary.mean_state_update_ms = state_update_total / static_cast<double>(report.samples.size());
    summary.mean_camera_world_ms = camera_world_total / static_cast<double>(report.samples.size());
    summary.mean_audio_ms = audio_total / static_cast<double>(report.samples.size());
    summary.mean_ui_ms = ui_total / static_cast<double>(report.samples.size());
    summary.mean_render_ms = mean(renders);
    summary.p95_render_ms = percentile(renders, 0.95);
    if (renderer_samples != 0) {
        const auto count = static_cast<double>(renderer_samples);
        summary.mean_renderer_cpu_ms = renderer_cpu_total / count;
        summary.mean_chunk_synchronization_ms = chunk_sync_total / count;
        summary.mean_render_extraction_ms = extraction_total / count;
        summary.mean_culling_ms = culling_total / count;
        summary.mean_draw_list_ms = draw_list_total / count;
        summary.mean_command_build_ms = command_build_total / count;
        summary.mean_command_recording_ms = command_recording_total / count;
        summary.mean_frontend_preparation_ms = frontend_preparation_total / count;
        summary.mean_lighting_preparation_ms = lighting_preparation_total / count;
        summary.mean_shadow_preparation_ms = shadow_preparation_total / count;
        summary.mean_chunk_draw_preparation_ms = chunk_draw_preparation_total / count;
        summary.mean_scene_preparation_ms = scene_preparation_total / count;
        summary.mean_shadow_command_build_ms = shadow_command_build_total / count;
        summary.mean_debug_ui_preparation_ms = debug_ui_preparation_total / count;
        summary.mean_backend_execute_ms = backend_execute_total / count;
        summary.mean_backend_frame_setup_ms = backend_frame_setup_total / count;
        summary.mean_queue_submit_ms = queue_submit_total / count;
        summary.mean_gpu_wait_ms = gpu_wait_total / count;
        summary.mean_frame_context_wait_ms = frame_context_wait_total / count;
        summary.mean_swapchain_acquire_ms = swapchain_acquire_total / count;
        summary.mean_queue_present_ms = queue_present_total / count;
    }
    if (summary.gpu_sample_count != 0) {
        summary.mean_gpu_ms = gpu_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_shadow_ms =
            gpu_shadow_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_sky_ms = gpu_sky_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_opaque_terrain_ms =
            gpu_opaque_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_alpha_tested_terrain_ms =
            gpu_alpha_tested_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_transparent_terrain_ms =
            gpu_transparent_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_tone_map_ms =
            gpu_tone_map_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_ui_ms = gpu_ui_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_transfer_ms =
            gpu_transfer_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_final_copy_ms =
            gpu_final_copy_total / static_cast<double>(summary.gpu_sample_count);
    }
    return summary;
}

std::string format_application_benchmark_summary(
    const GameApplicationBenchmarkSummary& summary) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << "n=" << summary.sample_count
           << " median=" << summary.median_frame_ms << "ms p95=" << summary.p95_frame_ms
           << "ms p99=" << summary.p99_frame_ms << "ms 1%low="
           << summary.one_percent_low_fps << "fps max=" << summary.maximum_frame_ms
           << "ms update=" << summary.mean_mode_update_ms << "ms render="
           << summary.mean_render_ms << "ms renderer_cpu=" << summary.mean_renderer_cpu_ms
           << "ms frontend=" << summary.mean_frontend_preparation_ms
           << "ms backend=" << summary.mean_backend_execute_ms << "ms gpu="
           << summary.mean_gpu_ms << "ms present_queue="
           << summary.mean_queue_present_ms << "ms";
    return output.str();
}

std::string application_benchmark_json(const GameApplicationBenchmarkMetadata& metadata,
                                       const GameApplicationBenchmarkReport& report) {
    const auto summary = summarize_application_benchmark(report);
    std::ostringstream output;
    output << std::fixed << std::setprecision(6)
           << "{\n  \"schema_version\": 1,\n"
           << "  \"workload\": \"" << json_escape(metadata.workload) << "\",\n"
           << "  \"source\": {\"engine_version\": \""
           << json_escape(metadata.runtime.engine_version) << "\", \"git_commit\": \""
           << json_escape(metadata.runtime.git_commit) << "\", \"git_dirty\": "
           << (metadata.runtime.git_dirty ? "true" : "false")
           << ", \"build_configuration\": \""
           << json_escape(metadata.runtime.build_configuration) << "\", \"compiler\": \""
           << json_escape(metadata.runtime.compiler) << "\", \"platform\": \""
           << json_escape(metadata.runtime.platform) << "\", \"architecture\": \""
           << json_escape(metadata.runtime.architecture) << "\", \"operating_system\": \""
           << json_escape(metadata.runtime.operating_system) << "\", \"cpu_model\": \""
           << json_escape(metadata.runtime.cpu_model) << "\", \"logical_cpu_count\": "
           << metadata.runtime.logical_cpu_count << ", \"tracy_enabled\": "
           << (metadata.runtime.tracy_enabled ? "true" : "false") << "},\n"
           << "  \"configuration\": {\"width\": " << report.width
           << ", \"height\": " << report.height << ", \"warmup_frames\": "
           << report.warmup_frames << ", \"measured_frames\": " << report.samples.size()
           << ", \"ready_frame\": " << report.ready_frame_index
           << ", \"validation_requested\": "
           << (report.validation_requested ? "true" : "false")
           << ", \"present_mode\": \"" << json_escape(report.present_mode)
           << "\", \"quality_preset\": \"" << json_escape(report.quality_preset)
           << "\"},\n"
           << "  \"device\": {\"backend\": \"" << json_escape(report.device.backend)
           << "\", \"name\": \"" << json_escape(report.device.device_name)
           << "\", \"driver\": \"" << json_escape(report.device.driver_name)
           << "\", \"driver_info\": \"" << json_escape(report.device.driver_info)
           << "\", \"vendor_id\": " << report.device.vendor_id
           << ", \"device_id\": " << report.device.device_id
           << ", \"api_version\": " << report.device.api_version
           << ", \"driver_version\": " << report.device.driver_version << "},\n"
           << "  \"summary\": {\"sample_count\": " << summary.sample_count
           << ", \"gpu_sample_count\": " << summary.gpu_sample_count
           << ", \"median_frame_ms\": " << summary.median_frame_ms
           << ", \"p95_frame_ms\": " << summary.p95_frame_ms
           << ", \"p99_frame_ms\": " << summary.p99_frame_ms
           << ", \"maximum_frame_ms\": " << summary.maximum_frame_ms
           << ", \"one_percent_low_fps\": " << summary.one_percent_low_fps
           << ", \"point_one_percent_low_fps\": " << summary.point_one_percent_low_fps
           << ", \"mean_event_pump_ms\": " << summary.mean_event_pump_ms
           << ", \"mean_mode_update_ms\": " << summary.mean_mode_update_ms
           << ", \"p95_mode_update_ms\": " << summary.p95_mode_update_ms
           << ", \"mean_state_update_ms\": " << summary.mean_state_update_ms
           << ", \"mean_camera_world_ms\": " << summary.mean_camera_world_ms
           << ", \"mean_audio_ms\": " << summary.mean_audio_ms
           << ", \"mean_ui_ms\": " << summary.mean_ui_ms
           << ", \"mean_render_ms\": " << summary.mean_render_ms
           << ", \"p95_render_ms\": " << summary.p95_render_ms
           << ", \"mean_renderer_cpu_ms\": " << summary.mean_renderer_cpu_ms
           << ", \"mean_gpu_ms\": " << summary.mean_gpu_ms
           << ", \"mean_frontend_preparation_ms\": "
           << summary.mean_frontend_preparation_ms
           << ", \"mean_lighting_preparation_ms\": "
           << summary.mean_lighting_preparation_ms
           << ", \"mean_shadow_preparation_ms\": " << summary.mean_shadow_preparation_ms
           << ", \"mean_chunk_draw_preparation_ms\": "
           << summary.mean_chunk_draw_preparation_ms
           << ", \"mean_scene_preparation_ms\": " << summary.mean_scene_preparation_ms
           << ", \"mean_shadow_command_build_ms\": "
           << summary.mean_shadow_command_build_ms
           << ", \"mean_debug_ui_preparation_ms\": "
           << summary.mean_debug_ui_preparation_ms
           << ", \"mean_backend_execute_ms\": " << summary.mean_backend_execute_ms
           << ", \"mean_backend_frame_setup_ms\": "
           << summary.mean_backend_frame_setup_ms
           << ", \"mean_chunk_synchronization_ms\": "
           << summary.mean_chunk_synchronization_ms
           << ", \"mean_render_extraction_ms\": " << summary.mean_render_extraction_ms
           << ", \"mean_culling_ms\": " << summary.mean_culling_ms
           << ", \"mean_draw_list_ms\": " << summary.mean_draw_list_ms
           << ", \"mean_command_build_ms\": " << summary.mean_command_build_ms
           << ", \"mean_command_recording_ms\": " << summary.mean_command_recording_ms
           << ", \"mean_queue_submit_ms\": " << summary.mean_queue_submit_ms
           << ", \"mean_gpu_wait_ms\": " << summary.mean_gpu_wait_ms
           << ", \"mean_frame_context_wait_ms\": "
           << summary.mean_frame_context_wait_ms
           << ", \"mean_swapchain_acquire_ms\": "
           << summary.mean_swapchain_acquire_ms
           << ", \"mean_queue_present_ms\": " << summary.mean_queue_present_ms
           << ", \"mean_gpu_shadow_ms\": " << summary.mean_gpu_shadow_ms
           << ", \"mean_gpu_sky_ms\": " << summary.mean_gpu_sky_ms
           << ", \"mean_gpu_opaque_terrain_ms\": "
           << summary.mean_gpu_opaque_terrain_ms
           << ", \"mean_gpu_alpha_tested_terrain_ms\": "
           << summary.mean_gpu_alpha_tested_terrain_ms
           << ", \"mean_gpu_transparent_terrain_ms\": "
           << summary.mean_gpu_transparent_terrain_ms
           << ", \"mean_gpu_tone_map_ms\": " << summary.mean_gpu_tone_map_ms
           << ", \"mean_gpu_ui_ms\": " << summary.mean_gpu_ui_ms
           << ", \"mean_gpu_transfer_ms\": " << summary.mean_gpu_transfer_ms
           << ", \"mean_gpu_final_copy_ms\": " << summary.mean_gpu_final_copy_ms
           << ", \"slowest_frame\": " << summary.slowest_frame_index << "},\n"
           << "  \"frames\": [\n";
    for (std::size_t index = 0; index < report.samples.size(); ++index) {
        const auto& sample = report.samples[index];
        output << "    {\"frame\": " << sample.frame_index
               << ", \"delta_us\": " << sample.delta_microseconds
               << ", \"total_ms\": " << sample.total_frame_ms
               << ", \"event_pump_ms\": " << sample.event_pump_ms
               << ", \"mode_update_ms\": " << sample.mode_update_ms
               << ", \"state_update_ms\": " << sample.mode.state_update_ms
               << ", \"camera_world_ms\": " << sample.mode.camera_world_ms
               << ", \"audio_ms\": " << sample.mode.audio_ms
               << ", \"ui_ms\": " << sample.mode.ui_ms
               << ", \"render_ms\": " << sample.render_ms;
        if (sample.renderer.has_value()) {
            const auto& renderer = *sample.renderer;
            output << ", \"renderer_cpu_ms\": " << renderer.cpu_frame_ms
                   << ", \"gpu_valid\": "
                   << (renderer.gpu_timing_valid ? "true" : "false")
                   << ", \"gpu_ms\": " << renderer.gpu_frame_ms
                   << ", \"chunk_sync_ms\": " << renderer.chunk_synchronization_ms
                   << ", \"extraction_ms\": " << renderer.render_extraction_ms
                   << ", \"culling_ms\": " << renderer.culling_ms
                   << ", \"draw_list_ms\": " << renderer.draw_list_ms
                   << ", \"command_build_ms\": " << renderer.command_build_ms
                   << ", \"frontend_preparation_ms\": "
                   << renderer.frontend_preparation_ms
                   << ", \"lighting_preparation_ms\": "
                   << renderer.lighting_preparation_ms
                   << ", \"shadow_preparation_ms\": " << renderer.shadow_preparation_ms
                   << ", \"chunk_draw_preparation_ms\": "
                   << renderer.chunk_draw_preparation_ms
                   << ", \"scene_preparation_ms\": " << renderer.scene_preparation_ms
                   << ", \"shadow_command_build_ms\": "
                   << renderer.shadow_command_build_ms
                   << ", \"debug_ui_preparation_ms\": "
                   << renderer.debug_ui_preparation_ms
                   << ", \"backend_execute_ms\": " << renderer.backend_execute_ms
                   << ", \"backend_frame_setup_ms\": "
                   << renderer.backend_frame_setup_ms
                   << ", \"command_recording_ms\": " << renderer.command_recording_ms
                   << ", \"queue_submit_ms\": " << renderer.queue_submit_ms
                   << ", \"gpu_wait_ms\": " << renderer.gpu_wait_ms
                   << ", \"frame_context_wait_ms\": "
                   << renderer.frame_context_wait_ms
                   << ", \"swapchain_acquire_ms\": "
                   << renderer.swapchain_acquire_ms
                   << ", \"queue_present_ms\": " << renderer.queue_present_ms
                   << ", \"gpu_shadow_ms\": " << renderer.gpu_shadow_ms
                   << ", \"gpu_sky_ms\": " << renderer.gpu_sky_ms
                   << ", \"gpu_opaque_terrain_ms\": "
                   << renderer.gpu_opaque_terrain_ms
                   << ", \"gpu_alpha_tested_terrain_ms\": "
                   << renderer.gpu_alpha_tested_terrain_ms
                   << ", \"gpu_transparent_terrain_ms\": "
                   << renderer.gpu_transparent_terrain_ms
                   << ", \"gpu_tone_map_ms\": " << renderer.gpu_tone_map_ms
                   << ", \"gpu_ui_ms\": " << renderer.gpu_ui_ms
                   << ", \"gpu_transfer_ms\": " << renderer.gpu_transfer_ms
                   << ", \"gpu_final_copy_ms\": " << renderer.gpu_final_copy_ms
                   << ", \"loaded_chunks\": " << renderer.loaded_chunks
                   << ", \"resident_chunks\": " << renderer.resident_chunks
                   << ", \"visible_chunks\": " << renderer.visible_chunks
                   << ", \"culled_chunks\": " << renderer.culled_chunks
                   << ", \"draw_calls\": " << renderer.draw_calls
                   << ", \"triangles\": " << renderer.triangles;
        }
        output << '}' << (index + 1U == report.samples.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

core::Status write_application_benchmark_json(
    const std::filesystem::path& path, const GameApplicationBenchmarkMetadata& metadata,
    const GameApplicationBenchmarkReport& report) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return core::Status::failure("game_application.benchmark_output_directory_failed",
                                         "failed to create benchmark output directory: " +
                                             error.message());
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Status::failure("game_application.benchmark_output_open_failed",
                                     "failed to open benchmark output: " + path.string());
    }
    output << application_benchmark_json(metadata, report);
    if (!output) {
        return core::Status::failure("game_application.benchmark_output_write_failed",
                                     "failed to write benchmark output: " + path.string());
    }
    return core::Status::ok();
}

} // namespace heartstead::game
