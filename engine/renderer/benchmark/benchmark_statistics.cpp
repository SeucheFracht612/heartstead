#include "engine/renderer/benchmark/benchmark_statistics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <utility>

namespace heartstead::renderer::benchmark {

namespace {

[[nodiscard]] double percentile(const std::vector<double>& sorted, double fraction) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto position = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const auto weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

[[nodiscard]] double low_fps(const std::vector<double>& sorted, double slow_fraction) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto count = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(static_cast<double>(sorted.size()) * slow_fraction)));
    const auto start = sorted.size() - count;
    const auto total =
        std::accumulate(sorted.begin() + static_cast<std::ptrdiff_t>(start), sorted.end(), 0.0);
    const auto mean_slow_frame_ms = total / static_cast<double>(count);
    return mean_slow_frame_ms > 0.0 ? 1'000.0 / mean_slow_frame_ms : 0.0;
}

[[nodiscard]] std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
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

[[nodiscard]] std::string csv_escape(std::string_view value) {
    std::string escaped{"\""};
    for (const char character : value) {
        if (character == '"') {
            escaped += "\"\"";
        } else {
            escaped += character;
        }
    }
    escaped += '"';
    return escaped;
}

[[nodiscard]] core::Status write_text_file(const std::filesystem::path& path,
                                           std::string_view text) {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return core::Status::failure("renderer.benchmark_create_directory_failed",
                                         "failed to create benchmark output directory: " +
                                             error.message());
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return core::Status::failure("renderer.benchmark_open_output_failed",
                                     "failed to open benchmark output file: " + path.string());
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        return core::Status::failure("renderer.benchmark_write_output_failed",
                                     "failed to write benchmark output file: " + path.string());
    }
    return core::Status::ok();
}

} // namespace

std::string_view benchmark_budget_profile_name(BenchmarkBudgetProfile profile) noexcept {
    switch (profile) {
    case BenchmarkBudgetProfile::none:
        return "none";
    case BenchmarkBudgetProfile::compatibility:
        return "compatibility";
    case BenchmarkBudgetProfile::minimum:
        return "minimum";
    case BenchmarkBudgetProfile::mainstream:
        return "mainstream";
    case BenchmarkBudgetProfile::high_end:
        return "high-end";
    }
    return "unknown";
}

std::optional<BenchmarkBudgetProfile>
parse_benchmark_budget_profile(std::string_view name) noexcept {
    constexpr std::array profiles{
        BenchmarkBudgetProfile::none,
        BenchmarkBudgetProfile::compatibility,
        BenchmarkBudgetProfile::minimum,
        BenchmarkBudgetProfile::mainstream,
        BenchmarkBudgetProfile::high_end,
    };
    const auto found = std::ranges::find_if(profiles, [name](BenchmarkBudgetProfile profile) {
        return benchmark_budget_profile_name(profile) == name;
    });
    return found == profiles.end() ? std::nullopt
                                   : std::optional<BenchmarkBudgetProfile>{*found};
}

std::optional<BenchmarkBudget> benchmark_budget(BenchmarkBudgetProfile profile) noexcept {
    const auto make_budget = [](double refresh_hz, double gpu_ms,
                                std::uint64_t upload_bytes) {
        const auto interval = 1'000.0 / refresh_hz;
        return BenchmarkBudget{
            interval,
            interval * 1.25,
            interval * 1.50,
            interval * 3.00,
            gpu_ms,
            upload_bytes,
        };
    };
    switch (profile) {
    case BenchmarkBudgetProfile::none:
        return std::nullopt;
    case BenchmarkBudgetProfile::compatibility:
        return make_budget(30.0, 28.0, 2U * 1024U * 1024U);
    case BenchmarkBudgetProfile::minimum:
        return make_budget(60.0, 13.5, 2U * 1024U * 1024U);
    case BenchmarkBudgetProfile::mainstream:
        return make_budget(90.0, 9.0, 4U * 1024U * 1024U);
    case BenchmarkBudgetProfile::high_end:
        return make_budget(120.0, 6.7, 4U * 1024U * 1024U);
    }
    return std::nullopt;
}

BenchmarkBudgetEvaluation evaluate_benchmark_budget(const BenchmarkSummary& summary,
                                                    BenchmarkBudgetProfile profile) {
    BenchmarkBudgetEvaluation evaluation;
    evaluation.profile = profile;
    const auto limits = benchmark_budget(profile);
    evaluation.limits = limits;
    if (!limits.has_value() || summary.sample_count == 0) {
        return evaluation;
    }
    evaluation.evaluated = true;
    const auto check = [&evaluation](std::string metric, double actual, double limit) {
        if (!std::isfinite(actual) || actual > limit) {
            evaluation.violations.push_back({std::move(metric), actual, limit});
        }
    };
    check("median_frame_ms", summary.median_frame_ms, limits->frame_interval_ms);
    check("p95_frame_ms", summary.p95_frame_ms, limits->maximum_p95_frame_ms);
    check("p99_frame_ms", summary.p99_frame_ms, limits->maximum_p99_frame_ms);
    check("maximum_frame_ms", summary.maximum_frame_ms, limits->maximum_frame_ms);
    check("maximum_uploaded_bytes", static_cast<double>(summary.maximum_uploaded_bytes),
          static_cast<double>(limits->maximum_upload_bytes_per_frame));
    if (summary.gpu_sample_count != 0) {
        evaluation.gpu_evaluated = true;
        check("mean_gpu_frame_ms", summary.mean_gpu_frame_ms, limits->maximum_mean_gpu_ms);
    }
    evaluation.passed = evaluation.violations.empty();
    return evaluation;
}

BenchmarkRecorder::BenchmarkRecorder(std::string scene, std::uint64_t seed)
    : metadata_{} {
    metadata_.scene = std::move(scene);
    metadata_.seed = seed;
}

BenchmarkRecorder::BenchmarkRecorder(BenchmarkRunMetadata metadata)
    : metadata_(std::move(metadata)) {}

void BenchmarkRecorder::record(RendererStats stats) {
    samples_.push_back(stats);
}

void BenchmarkRecorder::clear() noexcept {
    samples_.clear();
}

const std::vector<RendererStats>& BenchmarkRecorder::samples() const noexcept {
    return samples_;
}

const BenchmarkRunMetadata& BenchmarkRecorder::metadata() const noexcept {
    return metadata_;
}

BenchmarkSummary BenchmarkRecorder::summarize() const {
    BenchmarkSummary summary;
    summary.scene = metadata_.scene;
    summary.seed = metadata_.seed;
    summary.sample_count = samples_.size();
    if (samples_.empty()) {
        summary.budget = evaluate_benchmark_budget(summary, metadata_.budget_profile);
        return summary;
    }

    std::vector<double> frame_times;
    std::vector<double> relight_solve_times;
    std::vector<double> relight_apply_times;
    std::vector<double> fluid_snapshot_times;
    std::vector<double> fluid_simulation_times;
    std::vector<double> fluid_apply_times;
    std::vector<double> particle_update_times;
    std::vector<double> particle_presentation_times;
    frame_times.reserve(samples_.size());
    relight_solve_times.reserve(samples_.size());
    relight_apply_times.reserve(samples_.size());
    fluid_snapshot_times.reserve(samples_.size());
    fluid_simulation_times.reserve(samples_.size());
    fluid_apply_times.reserve(samples_.size());
    particle_update_times.reserve(samples_.size());
    particle_presentation_times.reserve(samples_.size());
    double cpu_total = 0.0;
    double gpu_total = 0.0;
    double gpu_upload_total = 0.0;
    double extraction_total = 0.0;
    double synchronization_total = 0.0;
    double culling_total = 0.0;
    double draw_list_total = 0.0;
    double command_build_total = 0.0;
    double command_recording_total = 0.0;
    double snapshot_total = 0.0;
    double meshing_total = 0.0;
    double upload_preparation_total = 0.0;
    double upload_total = 0.0;
    double gpu_wait_total = 0.0;
    double gpu_opaque_total = 0.0;
    double gpu_alpha_tested_total = 0.0;
    double gpu_transparent_total = 0.0;
    double gpu_transfer_total = 0.0;
    double gpu_final_copy_total = 0.0;
    for (const auto& sample : samples_) {
        frame_times.push_back(sample.cpu_frame_ms);
        relight_solve_times.push_back(sample.voxel_relight_solve_ms);
        relight_apply_times.push_back(sample.voxel_relight_apply_ms);
        fluid_snapshot_times.push_back(sample.voxel_fluid_snapshot_ms);
        fluid_simulation_times.push_back(sample.voxel_fluid_simulation_ms);
        fluid_apply_times.push_back(sample.voxel_fluid_apply_ms);
        particle_update_times.push_back(sample.particle_update_ms);
        particle_presentation_times.push_back(sample.particle_presentation_ms);
        cpu_total += sample.cpu_frame_ms;
        extraction_total += sample.render_extraction_ms;
        synchronization_total += sample.chunk_synchronization_ms;
        culling_total += sample.culling_ms;
        draw_list_total += sample.draw_list_ms;
        command_build_total += sample.command_build_ms;
        command_recording_total += sample.command_recording_ms;
        snapshot_total += sample.chunk_snapshot_ms;
        meshing_total += sample.meshing_ms;
        upload_preparation_total += sample.upload_preparation_ms;
        upload_total += sample.upload_ms;
        gpu_wait_total += sample.gpu_wait_ms;
        summary.total_uploaded_bytes += sample.uploaded_bytes_this_frame;
        summary.maximum_uploaded_bytes =
            std::max(summary.maximum_uploaded_bytes, sample.uploaded_bytes_this_frame);
        summary.maximum_voxel_relight_backlog_cells = std::max(
            summary.maximum_voxel_relight_backlog_cells, sample.voxel_relight_backlog_cells);
        summary.maximum_voxel_relight_visited_cells = std::max(
            summary.maximum_voxel_relight_visited_cells, sample.voxel_relight_visited_cells);
        summary.total_voxel_relight_changed_chunks += sample.voxel_relight_changed_chunks;
        summary.final_voxel_relight_stale_results =
            std::max(summary.final_voxel_relight_stale_results, sample.voxel_relight_stale_results);
        summary.final_voxel_relight_apply_budget_overruns =
            std::max(summary.final_voxel_relight_apply_budget_overruns,
                     sample.voxel_relight_apply_budget_overruns);
        summary.maximum_voxel_fluid_active_cells =
            std::max(summary.maximum_voxel_fluid_active_cells,
                     sample.voxel_fluid_active_cells);
        summary.maximum_voxel_fluid_processed_cells =
            std::max(summary.maximum_voxel_fluid_processed_cells,
                     sample.voxel_fluid_processed_cells);
        summary.total_voxel_fluid_changed_chunks += sample.voxel_fluid_changed_chunks;
        summary.final_voxel_fluid_budget_exhaustions =
            std::max(summary.final_voxel_fluid_budget_exhaustions,
                     sample.voxel_fluid_budget_exhaustions);
        summary.final_voxel_fluid_apply_budget_overruns =
            std::max(summary.final_voxel_fluid_apply_budget_overruns,
                     sample.voxel_fluid_apply_budget_overruns);
        summary.maximum_particle_active =
            std::max(summary.maximum_particle_active, sample.particle_active);
        summary.maximum_particle_material_groups = std::max(
            summary.maximum_particle_material_groups, sample.particle_material_groups);
        summary.final_particle_dropped =
            std::max(summary.final_particle_dropped, sample.particle_dropped);
        if (sample.gpu_timing_valid) {
            gpu_total += sample.gpu_frame_ms;
            gpu_opaque_total += sample.gpu_opaque_terrain_ms;
            gpu_alpha_tested_total += sample.gpu_alpha_tested_terrain_ms;
            gpu_transparent_total += sample.gpu_transparent_terrain_ms;
            gpu_transfer_total += sample.gpu_transfer_ms;
            gpu_final_copy_total += sample.gpu_final_copy_ms;
            ++summary.gpu_sample_count;
        }
        if (sample.gpu_upload_timing_valid) {
            gpu_upload_total += sample.gpu_upload_ms;
            ++summary.gpu_upload_sample_count;
        }
    }
    std::ranges::sort(frame_times);
    std::ranges::sort(relight_solve_times);
    std::ranges::sort(relight_apply_times);
    std::ranges::sort(fluid_snapshot_times);
    std::ranges::sort(fluid_simulation_times);
    std::ranges::sort(fluid_apply_times);
    std::ranges::sort(particle_update_times);
    std::ranges::sort(particle_presentation_times);
    summary.median_frame_ms = percentile(frame_times, 0.50);
    summary.p95_frame_ms = percentile(frame_times, 0.95);
    summary.p99_frame_ms = percentile(frame_times, 0.99);
    summary.one_percent_low_fps = low_fps(frame_times, 0.01);
    summary.point_one_percent_low_fps = low_fps(frame_times, 0.001);
    summary.maximum_frame_ms = frame_times.back();
    summary.median_voxel_relight_solve_ms = percentile(relight_solve_times, 0.50);
    summary.p95_voxel_relight_solve_ms = percentile(relight_solve_times, 0.95);
    summary.median_voxel_relight_apply_ms = percentile(relight_apply_times, 0.50);
    summary.p95_voxel_relight_apply_ms = percentile(relight_apply_times, 0.95);
    summary.median_voxel_fluid_snapshot_ms = percentile(fluid_snapshot_times, 0.50);
    summary.p95_voxel_fluid_snapshot_ms = percentile(fluid_snapshot_times, 0.95);
    summary.median_voxel_fluid_simulation_ms = percentile(fluid_simulation_times, 0.50);
    summary.p95_voxel_fluid_simulation_ms = percentile(fluid_simulation_times, 0.95);
    summary.median_voxel_fluid_apply_ms = percentile(fluid_apply_times, 0.50);
    summary.p95_voxel_fluid_apply_ms = percentile(fluid_apply_times, 0.95);
    summary.median_particle_update_ms = percentile(particle_update_times, 0.50);
    summary.p95_particle_update_ms = percentile(particle_update_times, 0.95);
    summary.median_particle_presentation_ms = percentile(particle_presentation_times, 0.50);
    summary.p95_particle_presentation_ms = percentile(particle_presentation_times, 0.95);
    summary.slowest_frame = *std::ranges::max_element(
        samples_, {}, [](const RendererStats& sample) { return sample.cpu_frame_ms; });
    const auto sample_count = static_cast<double>(samples_.size());
    summary.mean_cpu_frame_ms = cpu_total / sample_count;
    summary.mean_render_extraction_ms = extraction_total / sample_count;
    summary.mean_chunk_synchronization_ms = synchronization_total / sample_count;
    summary.mean_culling_ms = culling_total / sample_count;
    summary.mean_draw_list_ms = draw_list_total / sample_count;
    summary.mean_command_build_ms = command_build_total / sample_count;
    summary.mean_command_recording_ms = command_recording_total / sample_count;
    summary.mean_chunk_snapshot_ms = snapshot_total / sample_count;
    summary.mean_meshing_ms = meshing_total / sample_count;
    summary.mean_upload_preparation_ms = upload_preparation_total / sample_count;
    summary.mean_upload_ms = upload_total / sample_count;
    summary.mean_gpu_wait_ms = gpu_wait_total / sample_count;
    if (summary.gpu_sample_count != 0) {
        summary.mean_gpu_frame_ms = gpu_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_opaque_terrain_ms =
            gpu_opaque_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_alpha_tested_terrain_ms =
            gpu_alpha_tested_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_transparent_terrain_ms =
            gpu_transparent_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_transfer_ms =
            gpu_transfer_total / static_cast<double>(summary.gpu_sample_count);
        summary.mean_gpu_final_copy_ms =
            gpu_final_copy_total / static_cast<double>(summary.gpu_sample_count);
    }
    if (summary.gpu_upload_sample_count != 0) {
        summary.mean_gpu_upload_ms =
            gpu_upload_total / static_cast<double>(summary.gpu_upload_sample_count);
    }
    summary.budget = evaluate_benchmark_budget(summary, metadata_.budget_profile);
    return summary;
}

std::string BenchmarkRecorder::to_json() const {
    const auto summary = summarize();
    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{\n  \"schema\": \"heartstead.renderer_benchmark.v2\",\n"
           << "  \"scene\": \"" << json_escape(metadata_.scene) << "\",\n"
           << "  \"seed\": " << metadata_.seed << ",\n"
           << "  \"provenance\": {\n"
           << "    \"engine_version\": \"" << json_escape(metadata_.engine_version) << "\",\n"
           << "    \"git_commit\": \"" << json_escape(metadata_.git_commit) << "\",\n"
           << "    \"git_dirty\": " << (metadata_.git_dirty ? "true" : "false") << ",\n"
           << "    \"build_configuration\": \""
           << json_escape(metadata_.build_configuration) << "\",\n"
           << "    \"compiler\": \"" << json_escape(metadata_.compiler) << "\",\n"
           << "    \"platform\": \"" << json_escape(metadata_.platform) << "\",\n"
           << "    \"architecture\": \"" << json_escape(metadata_.architecture) << "\",\n"
           << "    \"operating_system\": \"" << json_escape(metadata_.operating_system)
           << "\",\n"
           << "    \"cpu_model\": \"" << json_escape(metadata_.cpu_model) << "\",\n"
           << "    \"logical_cpu_count\": " << metadata_.logical_cpu_count << ",\n"
           << "    \"tracy_enabled\": " << (metadata_.tracy_enabled ? "true" : "false")
           << ",\n"
           << "    \"gpu_name\": \"" << json_escape(metadata_.gpu_name) << "\",\n"
           << "    \"gpu_driver\": \"" << json_escape(metadata_.gpu_driver) << "\",\n"
           << "    \"gpu_driver_info\": \"" << json_escape(metadata_.gpu_driver_info)
           << "\",\n"
           << "    \"gpu_vendor_id\": " << metadata_.gpu_vendor_id << ",\n"
           << "    \"gpu_device_id\": " << metadata_.gpu_device_id << ",\n"
           << "    \"graphics_api_version\": " << metadata_.graphics_api_version << ",\n"
           << "    \"graphics_driver_version\": " << metadata_.graphics_driver_version << "\n"
           << "  },\n"
           << "  \"run\": {\n"
           << "    \"backend\": \"" << json_escape(metadata_.backend) << "\",\n"
           << "    \"mesher\": \"" << json_escape(metadata_.mesher) << "\",\n"
           << "    \"initial_width\": " << metadata_.initial_width << ",\n"
           << "    \"initial_height\": " << metadata_.initial_height << ",\n"
           << "    \"chunk_radius\": " << metadata_.chunk_radius << ",\n"
           << "    \"warmup_frames\": " << metadata_.warmup_frames << ",\n"
           << "    \"measured_frames\": " << metadata_.measured_frames << ",\n"
           << "    \"frame_cap\": " << metadata_.frame_cap << ",\n"
           << "    \"budget_profile\": \""
           << benchmark_budget_profile_name(metadata_.budget_profile) << "\",\n"
           << "    \"validation_requested\": "
           << (metadata_.validation_requested ? "true" : "false") << "\n"
           << "  },\n"
           << "  \"summary\": {\n"
           << "    \"sample_count\": " << summary.sample_count << ",\n"
           << "    \"gpu_sample_count\": " << summary.gpu_sample_count << ",\n"
           << "    \"gpu_upload_sample_count\": " << summary.gpu_upload_sample_count << ",\n"
           << "    \"median_frame_ms\": " << summary.median_frame_ms << ",\n"
           << "    \"p95_frame_ms\": " << summary.p95_frame_ms << ",\n"
           << "    \"p99_frame_ms\": " << summary.p99_frame_ms << ",\n"
           << "    \"one_percent_low_fps\": " << summary.one_percent_low_fps << ",\n"
           << "    \"point_one_percent_low_fps\": " << summary.point_one_percent_low_fps << ",\n"
           << "    \"maximum_frame_ms\": " << summary.maximum_frame_ms << ",\n"
           << "    \"mean_cpu_frame_ms\": " << summary.mean_cpu_frame_ms << ",\n"
           << "    \"mean_gpu_frame_ms\": " << summary.mean_gpu_frame_ms << ",\n"
           << "    \"mean_gpu_upload_ms\": " << summary.mean_gpu_upload_ms << ",\n"
           << "    \"mean_render_extraction_ms\": " << summary.mean_render_extraction_ms << ",\n"
           << "    \"mean_chunk_synchronization_ms\": " << summary.mean_chunk_synchronization_ms
           << ",\n"
           << "    \"mean_culling_ms\": " << summary.mean_culling_ms << ",\n"
           << "    \"mean_draw_list_ms\": " << summary.mean_draw_list_ms << ",\n"
           << "    \"mean_command_build_ms\": " << summary.mean_command_build_ms << ",\n"
           << "    \"mean_command_recording_ms\": " << summary.mean_command_recording_ms << ",\n"
           << "    \"mean_chunk_snapshot_ms\": " << summary.mean_chunk_snapshot_ms << ",\n"
           << "    \"mean_meshing_ms\": " << summary.mean_meshing_ms << ",\n"
           << "    \"mean_upload_preparation_ms\": " << summary.mean_upload_preparation_ms << ",\n"
           << "    \"mean_upload_ms\": " << summary.mean_upload_ms << ",\n"
           << "    \"mean_gpu_wait_ms\": " << summary.mean_gpu_wait_ms << ",\n"
           << "    \"mean_gpu_opaque_terrain_ms\": " << summary.mean_gpu_opaque_terrain_ms << ",\n"
           << "    \"mean_gpu_alpha_tested_terrain_ms\": "
           << summary.mean_gpu_alpha_tested_terrain_ms << ",\n"
           << "    \"mean_gpu_transparent_terrain_ms\": " << summary.mean_gpu_transparent_terrain_ms
           << ",\n"
           << "    \"mean_gpu_transfer_ms\": " << summary.mean_gpu_transfer_ms << ",\n"
           << "    \"mean_gpu_final_copy_ms\": " << summary.mean_gpu_final_copy_ms << ",\n"
           << "    \"median_voxel_relight_solve_ms\": " << summary.median_voxel_relight_solve_ms
           << ",\n"
           << "    \"p95_voxel_relight_solve_ms\": " << summary.p95_voxel_relight_solve_ms << ",\n"
           << "    \"median_voxel_relight_apply_ms\": " << summary.median_voxel_relight_apply_ms
           << ",\n"
           << "    \"p95_voxel_relight_apply_ms\": " << summary.p95_voxel_relight_apply_ms << ",\n"
           << "    \"maximum_voxel_relight_backlog_cells\": "
           << summary.maximum_voxel_relight_backlog_cells << ",\n"
           << "    \"maximum_voxel_relight_visited_cells\": "
           << summary.maximum_voxel_relight_visited_cells << ",\n"
           << "    \"total_voxel_relight_changed_chunks\": "
           << summary.total_voxel_relight_changed_chunks << ",\n"
           << "    \"final_voxel_relight_stale_results\": "
           << summary.final_voxel_relight_stale_results << ",\n"
           << "    \"final_voxel_relight_apply_budget_overruns\": "
           << summary.final_voxel_relight_apply_budget_overruns << ",\n"
           << "    \"median_voxel_fluid_snapshot_ms\": "
           << summary.median_voxel_fluid_snapshot_ms << ",\n"
           << "    \"p95_voxel_fluid_snapshot_ms\": "
           << summary.p95_voxel_fluid_snapshot_ms << ",\n"
           << "    \"median_voxel_fluid_simulation_ms\": "
           << summary.median_voxel_fluid_simulation_ms << ",\n"
           << "    \"p95_voxel_fluid_simulation_ms\": "
           << summary.p95_voxel_fluid_simulation_ms << ",\n"
           << "    \"median_voxel_fluid_apply_ms\": "
           << summary.median_voxel_fluid_apply_ms << ",\n"
           << "    \"p95_voxel_fluid_apply_ms\": "
           << summary.p95_voxel_fluid_apply_ms << ",\n"
           << "    \"maximum_voxel_fluid_active_cells\": "
           << summary.maximum_voxel_fluid_active_cells << ",\n"
           << "    \"maximum_voxel_fluid_processed_cells\": "
           << summary.maximum_voxel_fluid_processed_cells << ",\n"
           << "    \"total_voxel_fluid_changed_chunks\": "
           << summary.total_voxel_fluid_changed_chunks << ",\n"
           << "    \"final_voxel_fluid_budget_exhaustions\": "
           << summary.final_voxel_fluid_budget_exhaustions << ",\n"
           << "    \"final_voxel_fluid_apply_budget_overruns\": "
           << summary.final_voxel_fluid_apply_budget_overruns << ",\n"
           << "    \"median_particle_update_ms\": " << summary.median_particle_update_ms << ",\n"
           << "    \"p95_particle_update_ms\": " << summary.p95_particle_update_ms << ",\n"
           << "    \"median_particle_presentation_ms\": "
           << summary.median_particle_presentation_ms << ",\n"
           << "    \"p95_particle_presentation_ms\": "
           << summary.p95_particle_presentation_ms << ",\n"
           << "    \"maximum_particle_active\": " << summary.maximum_particle_active << ",\n"
           << "    \"maximum_particle_material_groups\": "
           << summary.maximum_particle_material_groups << ",\n"
           << "    \"final_particle_dropped\": " << summary.final_particle_dropped << ",\n"
           << "    \"total_uploaded_bytes\": " << summary.total_uploaded_bytes << ",\n"
           << "    \"maximum_uploaded_bytes\": " << summary.maximum_uploaded_bytes << ",\n"
           << "    \"budget\": {\"profile\": \""
           << benchmark_budget_profile_name(summary.budget.profile) << "\", \"evaluated\": "
           << (summary.budget.evaluated ? "true" : "false") << ", \"passed\": "
           << (summary.budget.passed ? "true" : "false") << ", \"gpu_evaluated\": "
           << (summary.budget.gpu_evaluated ? "true" : "false") << ", \"limits\": ";
    if (summary.budget.limits.has_value()) {
        const auto& limits = *summary.budget.limits;
        output << "{\"frame_interval_ms\": " << limits.frame_interval_ms
               << ", \"maximum_p95_frame_ms\": " << limits.maximum_p95_frame_ms
               << ", \"maximum_p99_frame_ms\": " << limits.maximum_p99_frame_ms
               << ", \"maximum_frame_ms\": " << limits.maximum_frame_ms
               << ", \"maximum_mean_gpu_ms\": " << limits.maximum_mean_gpu_ms
               << ", \"maximum_upload_bytes_per_frame\": "
               << limits.maximum_upload_bytes_per_frame << '}';
    } else {
        output << "null";
    }
    output << ", \"violations\": [";
    for (std::size_t index = 0; index < summary.budget.violations.size(); ++index) {
        const auto& violation = summary.budget.violations[index];
        output << "{\"metric\": \"" << json_escape(violation.metric) << "\", \"actual\": "
               << violation.actual << ", \"limit\": " << violation.limit << "}"
               << (index + 1 == summary.budget.violations.size() ? "" : ", ");
    }
    output << "]},\n"
           << "    \"slowest_frame\": {\"frame\": " << summary.slowest_frame.frame_index
           << ", \"cpu_frame_ms\": " << summary.slowest_frame.cpu_frame_ms
           << ", \"extraction_ms\": " << summary.slowest_frame.render_extraction_ms
           << ", \"sync_ms\": " << summary.slowest_frame.chunk_synchronization_ms
           << ", \"culling_ms\": " << summary.slowest_frame.culling_ms
           << ", \"draw_list_ms\": " << summary.slowest_frame.draw_list_ms
           << ", \"command_build_ms\": " << summary.slowest_frame.command_build_ms
           << ", \"command_recording_ms\": " << summary.slowest_frame.command_recording_ms
           << ", \"snapshot_ms\": " << summary.slowest_frame.chunk_snapshot_ms
           << ", \"meshing_ms\": " << summary.slowest_frame.meshing_ms
           << ", \"upload_preparation_ms\": " << summary.slowest_frame.upload_preparation_ms
           << ", \"upload_ms\": " << summary.slowest_frame.upload_ms
           << ", \"gpu_wait_ms\": " << summary.slowest_frame.gpu_wait_ms << "}\n"
           << "  },\n  \"frames\": [\n";
    for (std::size_t index = 0; index < samples_.size(); ++index) {
        const auto& sample = samples_[index];
        output << "    {\"frame\": " << sample.frame_index
               << ", \"submission_serial\": " << sample.submission_serial
               << ", \"completed_submission_serial\": " << sample.completed_submission_serial
               << ", \"cpu_frame_ms\": " << sample.cpu_frame_ms
               << ", \"gpu_valid\": " << (sample.gpu_timing_valid ? "true" : "false")
               << ", \"gpu_timing_frame\": " << sample.gpu_timing_frame_index
               << ", \"gpu_latency_frames\": " << sample.gpu_timing_latency_frames
               << ", \"gpu_frame_ms\": " << sample.gpu_frame_ms
               << ", \"gpu_upload_valid\": " << (sample.gpu_upload_timing_valid ? "true" : "false")
               << ", \"gpu_upload_submission_serial\": " << sample.gpu_upload_submission_serial
               << ", \"gpu_upload_ms\": " << sample.gpu_upload_ms
               << ", \"gpu_opaque_ms\": " << sample.gpu_opaque_terrain_ms
               << ", \"gpu_alpha_tested_ms\": " << sample.gpu_alpha_tested_terrain_ms
               << ", \"gpu_transparent_ms\": " << sample.gpu_transparent_terrain_ms
               << ", \"gpu_transfer_ms\": " << sample.gpu_transfer_ms
               << ", \"gpu_final_copy_ms\": " << sample.gpu_final_copy_ms
               << ", \"extraction_ms\": " << sample.render_extraction_ms
               << ", \"sync_ms\": " << sample.chunk_synchronization_ms
               << ", \"culling_ms\": " << sample.culling_ms
               << ", \"draw_list_ms\": " << sample.draw_list_ms
               << ", \"command_build_ms\": " << sample.command_build_ms
               << ", \"command_recording_ms\": " << sample.command_recording_ms
               << ", \"snapshot_ms\": " << sample.chunk_snapshot_ms
               << ", \"meshing_ms\": " << sample.meshing_ms
               << ", \"upload_preparation_ms\": " << sample.upload_preparation_ms
               << ", \"upload_ms\": " << sample.upload_ms
               << ", \"gpu_wait_ms\": " << sample.gpu_wait_ms
               << ", \"voxel_relight_solve_ms\": " << sample.voxel_relight_solve_ms
               << ", \"voxel_relight_apply_ms\": " << sample.voxel_relight_apply_ms
               << ", \"voxel_relight_backlog_cells\": " << sample.voxel_relight_backlog_cells
               << ", \"voxel_relight_visited_cells\": " << sample.voxel_relight_visited_cells
               << ", \"voxel_relight_changed_chunks\": " << sample.voxel_relight_changed_chunks
               << ", \"voxel_relight_stale_results\": " << sample.voxel_relight_stale_results
               << ", \"voxel_relight_apply_budget_overruns\": "
               << sample.voxel_relight_apply_budget_overruns
               << ", \"voxel_fluid_snapshot_ms\": " << sample.voxel_fluid_snapshot_ms
               << ", \"voxel_fluid_simulation_ms\": " << sample.voxel_fluid_simulation_ms
               << ", \"voxel_fluid_apply_ms\": " << sample.voxel_fluid_apply_ms
               << ", \"voxel_fluid_active_cells\": " << sample.voxel_fluid_active_cells
               << ", \"voxel_fluid_processed_cells\": " << sample.voxel_fluid_processed_cells
               << ", \"voxel_fluid_changed_chunks\": " << sample.voxel_fluid_changed_chunks
               << ", \"voxel_fluid_budget_exhaustions\": "
               << sample.voxel_fluid_budget_exhaustions
               << ", \"voxel_fluid_apply_budget_overruns\": "
               << sample.voxel_fluid_apply_budget_overruns
               << ", \"particle_update_ms\": " << sample.particle_update_ms
               << ", \"particle_presentation_ms\": " << sample.particle_presentation_ms
               << ", \"particle_active\": " << sample.particle_active
               << ", \"particle_emitters\": " << sample.particle_emitters
               << ", \"particle_spawned\": " << sample.particle_spawned
               << ", \"particle_material_groups\": " << sample.particle_material_groups
               << ", \"particle_dropped\": " << sample.particle_dropped
               << ", \"loaded_chunks\": " << sample.loaded_chunks
               << ", \"mesh_pending_chunks\": " << sample.mesh_pending_chunks
               << ", \"upload_pending_chunks\": " << sample.upload_pending_chunks
               << ", \"resident_chunks\": " << sample.resident_chunks
               << ", \"visible_chunks\": " << sample.visible_chunks
               << ", \"culled_chunks\": " << sample.culled_chunks
               << ", \"drawn_chunks\": " << sample.drawn_chunks
               << ", \"far_terrain_planned_patches\": "
               << sample.far_terrain_planned_patches
               << ", \"far_terrain_resident_patches\": "
               << sample.far_terrain_resident_patches
               << ", \"far_terrain_visible_patches\": "
               << sample.far_terrain_visible_patches
               << ", \"far_terrain_pending_patches\": "
               << sample.far_terrain_pending_patches
               << ", \"far_terrain_evicted_patches\": "
               << sample.far_terrain_evicted_patches
               << ", \"draw_calls\": " << sample.draw_calls
               << ", \"indirect_draw_calls\": " << sample.indirect_draw_calls
               << ", \"opaque_terrain_draws\": " << sample.opaque_terrain_draws
               << ", \"alpha_tested_terrain_draws\": " << sample.alpha_tested_terrain_draws
               << ", \"transparent_terrain_draws\": " << sample.transparent_terrain_draws
               << ", \"pipeline_switches\": " << sample.pipeline_switches
               << ", \"resident_textures\": " << sample.resident_textures
               << ", \"runtime_materials\": " << sample.runtime_materials
               << ", \"resident_pipelines\": " << sample.resident_pipelines
               << ", \"retained_objects\": " << sample.retained_objects
               << ", \"visible_objects\": " << sample.visible_objects
               << ", \"culled_objects\": " << sample.culled_objects
               << ", \"visibility_hierarchy_nodes\": "
               << sample.visibility_hierarchy_nodes
               << ", \"visibility_nodes_tested\": " << sample.visibility_nodes_tested
               << ", \"visibility_nodes_culled\": " << sample.visibility_nodes_culled
               << ", \"submitted_instances\": " << sample.submitted_instances
               << ", \"instance_draw_calls\": " << sample.instance_draw_calls
               << ", \"debug_lines\": " << sample.debug_lines
               << ", \"debug_draw_calls\": " << sample.debug_draw_calls
               << ", \"debug_labels\": " << sample.debug_labels
               << ", \"ui_vertices\": " << sample.ui_vertices
               << ", \"ui_glyphs\": " << sample.ui_glyphs
               << ", \"ui_draw_calls\": " << sample.ui_draw_calls
               << ", \"ui_clipped_draw_calls\": " << sample.ui_clipped_draw_calls
               << ", \"vertices\": " << sample.vertices << ", \"triangles\": " << sample.triangles
               << ", \"resident_texture_bytes\": " << sample.resident_texture_bytes
               << ", \"resident_mesh_bytes\": " << sample.resident_mesh_bytes
               << ", \"far_terrain_resident_bytes\": "
               << sample.far_terrain_resident_bytes
               << ", \"far_terrain_uploaded_bytes\": "
               << sample.far_terrain_uploaded_bytes
               << ", \"device_memory_budget_valid\": "
               << (sample.device_memory_budget_valid ? "true" : "false")
               << ", \"device_local_memory_budget_bytes\": "
               << sample.device_local_memory_budget_bytes
               << ", \"device_local_memory_usage_bytes\": "
               << sample.device_local_memory_usage_bytes
               << ", \"gpu_arena_capacity_bytes\": " << sample.gpu_arena_capacity_bytes
               << ", \"gpu_arena_used_bytes\": " << sample.gpu_arena_used_bytes
               << ", \"gpu_arena_free_bytes\": " << sample.gpu_arena_free_bytes
               << ", \"gpu_arena_fragmentation\": " << sample.gpu_arena_fragmentation
               << ", \"pending_upload_bytes\": " << sample.pending_upload_bytes
               << ", \"uploaded_bytes\": " << sample.uploaded_bytes_this_frame << "}";
        output << (index + 1 == samples_.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

std::string BenchmarkRecorder::to_csv() const {
    const auto summary = summarize();
    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "schema,scene,seed,engine_version,git_commit,git_dirty,build_configuration,compiler,"
              "platform,"
              "architecture,operating_system,cpu_model,logical_cpu_count,tracy_enabled,gpu_name,"
              "gpu_driver,gpu_driver_info,gpu_vendor_id,gpu_device_id,graphics_api_version,"
              "graphics_driver_version,backend,mesher,initial_width,initial_height,chunk_radius,"
              "warmup_frames,measured_frames,frame_cap,budget_profile,budget_frame_interval_ms,"
              "budget_maximum_p95_frame_ms,budget_maximum_p99_frame_ms,"
              "budget_maximum_frame_ms,budget_maximum_mean_gpu_ms,"
              "budget_maximum_upload_bytes_per_frame,budget_evaluated,budget_passed,"
              "budget_violation_count,validation_requested,median_frame_ms,p95_frame_ms,"
              "p99_frame_ms,one_percent_low_fps,point_one_percent_low_fps,maximum_frame_ms,"
              "frame,submission_serial,completed_submission_serial,cpu_frame_ms,"
              "gpu_valid,gpu_timing_frame,gpu_latency_frames,gpu_frame_ms,gpu_upload_valid,"
              "gpu_upload_submission_serial,gpu_upload_ms,gpu_opaque_ms,gpu_alpha_tested_ms,"
              "gpu_transparent_ms,gpu_transfer_ms,gpu_final_copy_ms,extraction_ms,"
              "sync_ms,culling_ms,draw_list_ms,command_build_ms,command_recording_ms,snapshot_ms,"
              "meshing_ms,upload_preparation_ms,upload_ms,gpu_wait_ms,"
              "voxel_relight_solve_ms,voxel_relight_apply_ms,voxel_relight_backlog_cells,"
              "voxel_relight_visited_cells,voxel_relight_changed_chunks,"
              "voxel_relight_stale_results,voxel_relight_apply_budget_overruns,"
              "voxel_fluid_snapshot_ms,voxel_fluid_simulation_ms,voxel_fluid_apply_ms,"
              "voxel_fluid_active_cells,voxel_fluid_processed_cells,"
              "voxel_fluid_changed_chunks,voxel_fluid_budget_exhaustions,"
              "voxel_fluid_apply_budget_overruns,loaded_chunks,"
              "mesh_pending_chunks,upload_pending_chunks,resident_chunks,visible_chunks,"
              "culled_chunks,drawn_chunks,far_terrain_planned_patches,"
              "far_terrain_resident_patches,far_terrain_visible_patches,"
              "far_terrain_pending_patches,far_terrain_evicted_patches,"
              "draw_calls,indirect_draw_calls,opaque_terrain_draws,"
              "alpha_tested_terrain_draws,transparent_terrain_draws,pipeline_switches,"
              "resident_textures,"
              "runtime_materials,resident_pipelines,retained_objects,visible_objects,"
              "culled_objects,visibility_hierarchy_nodes,visibility_nodes_tested,"
              "visibility_nodes_culled,submitted_instances,instance_draw_calls,debug_lines,"
              "debug_draw_calls,debug_labels,ui_vertices,ui_glyphs,ui_draw_calls,"
              "ui_clipped_draw_calls,vertices,triangles,resident_texture_bytes,"
              "resident_mesh_bytes,far_terrain_resident_bytes,far_terrain_uploaded_bytes,"
              "device_memory_budget_valid,device_local_memory_budget_bytes,"
              "device_local_memory_usage_bytes,"
              "gpu_arena_capacity_bytes,gpu_arena_used_bytes,gpu_arena_free_bytes,"
              "gpu_arena_fragmentation,pending_upload_bytes,uploaded_bytes\n";
    const auto limits = summary.budget.limits.value_or(BenchmarkBudget{});
    for (const auto& sample : samples_) {
        output << csv_escape("heartstead.renderer_benchmark.v2") << ','
               << csv_escape(metadata_.scene) << ',' << metadata_.seed << ','
               << csv_escape(metadata_.engine_version) << ',' << csv_escape(metadata_.git_commit)
               << ',' << (metadata_.git_dirty ? 1 : 0) << ','
               << csv_escape(metadata_.build_configuration) << ',' << csv_escape(metadata_.compiler)
               << ',' << csv_escape(metadata_.platform) << ',' << csv_escape(metadata_.architecture)
               << ',' << csv_escape(metadata_.operating_system) << ','
               << csv_escape(metadata_.cpu_model) << ',' << metadata_.logical_cpu_count << ','
               << (metadata_.tracy_enabled ? 1 : 0) << ',' << csv_escape(metadata_.gpu_name) << ','
               << csv_escape(metadata_.gpu_driver) << ',' << csv_escape(metadata_.gpu_driver_info)
               << ',' << metadata_.gpu_vendor_id << ',' << metadata_.gpu_device_id << ','
               << metadata_.graphics_api_version << ',' << metadata_.graphics_driver_version << ','
               << csv_escape(metadata_.backend) << ',' << csv_escape(metadata_.mesher) << ','
               << metadata_.initial_width << ',' << metadata_.initial_height << ','
               << metadata_.chunk_radius << ',' << metadata_.warmup_frames << ','
               << metadata_.measured_frames << ',' << metadata_.frame_cap << ','
               << csv_escape(benchmark_budget_profile_name(metadata_.budget_profile)) << ','
               << limits.frame_interval_ms << ',' << limits.maximum_p95_frame_ms << ','
               << limits.maximum_p99_frame_ms << ',' << limits.maximum_frame_ms << ','
               << limits.maximum_mean_gpu_ms << ',' << limits.maximum_upload_bytes_per_frame << ','
               << (summary.budget.evaluated ? 1 : 0) << ',' << (summary.budget.passed ? 1 : 0) << ','
               << summary.budget.violations.size() << ','
               << (metadata_.validation_requested ? 1 : 0) << ',' << summary.median_frame_ms << ','
               << summary.p95_frame_ms << ',' << summary.p99_frame_ms << ','
               << summary.one_percent_low_fps << ',' << summary.point_one_percent_low_fps << ','
               << summary.maximum_frame_ms << ',' << sample.frame_index << ','
               << sample.submission_serial << ',' << sample.completed_submission_serial << ','
               << sample.cpu_frame_ms << ',' << (sample.gpu_timing_valid ? 1 : 0) << ','
               << sample.gpu_timing_frame_index << ',' << sample.gpu_timing_latency_frames << ','
               << sample.gpu_frame_ms << ',' << (sample.gpu_upload_timing_valid ? 1 : 0) << ','
               << sample.gpu_upload_submission_serial << ',' << sample.gpu_upload_ms << ','
               << sample.gpu_opaque_terrain_ms << ',' << sample.gpu_alpha_tested_terrain_ms << ','
               << sample.gpu_transparent_terrain_ms << ',' << sample.gpu_transfer_ms << ','
               << sample.gpu_final_copy_ms << ',' << sample.render_extraction_ms << ','
               << sample.chunk_synchronization_ms << ',' << sample.culling_ms << ','
               << sample.draw_list_ms << ',' << sample.command_build_ms << ','
               << sample.command_recording_ms << ',' << sample.chunk_snapshot_ms << ','
               << sample.meshing_ms << ',' << sample.upload_preparation_ms << ','
               << sample.upload_ms << ',' << sample.gpu_wait_ms << ','
               << sample.voxel_relight_solve_ms << ',' << sample.voxel_relight_apply_ms << ','
               << sample.voxel_relight_backlog_cells << ',' << sample.voxel_relight_visited_cells
               << ',' << sample.voxel_relight_changed_chunks << ','
               << sample.voxel_relight_stale_results << ','
               << sample.voxel_relight_apply_budget_overruns << ','
               << sample.voxel_fluid_snapshot_ms << ',' << sample.voxel_fluid_simulation_ms << ','
               << sample.voxel_fluid_apply_ms << ',' << sample.voxel_fluid_active_cells << ','
               << sample.voxel_fluid_processed_cells << ',' << sample.voxel_fluid_changed_chunks
               << ',' << sample.voxel_fluid_budget_exhaustions << ','
               << sample.voxel_fluid_apply_budget_overruns << ',' << sample.loaded_chunks << ','
               << sample.mesh_pending_chunks << ',' << sample.upload_pending_chunks << ','
               << sample.resident_chunks << ',' << sample.visible_chunks << ','
               << sample.culled_chunks << ',' << sample.drawn_chunks
               << ',' << sample.far_terrain_planned_patches << ','
               << sample.far_terrain_resident_patches << ','
               << sample.far_terrain_visible_patches << ','
               << sample.far_terrain_pending_patches << ','
               << sample.far_terrain_evicted_patches
               << ',' << sample.draw_calls << ',' << sample.indirect_draw_calls << ','
               << sample.opaque_terrain_draws << ','
               << sample.alpha_tested_terrain_draws
               << ',' << sample.transparent_terrain_draws << ',' << sample.pipeline_switches << ','
               << sample.resident_textures << ',' << sample.runtime_materials << ','
               << sample.resident_pipelines << ',' << sample.retained_objects << ','
               << sample.visible_objects << ',' << sample.culled_objects << ','
               << sample.visibility_hierarchy_nodes << ',' << sample.visibility_nodes_tested << ','
               << sample.visibility_nodes_culled << ',' << sample.submitted_instances << ','
               << sample.instance_draw_calls << ','
               << sample.debug_lines << ',' << sample.debug_draw_calls << ',' << sample.debug_labels
               << ',' << sample.ui_vertices << ',' << sample.ui_glyphs << ','
               << sample.ui_draw_calls << ',' << sample.ui_clipped_draw_calls << ','
               << sample.vertices << ',' << sample.triangles << ',' << sample.resident_texture_bytes
               << ',' << sample.resident_mesh_bytes << ',' << sample.far_terrain_resident_bytes
               << ',' << sample.far_terrain_uploaded_bytes << ','
               << (sample.device_memory_budget_valid ? 1 : 0) << ','
               << sample.device_local_memory_budget_bytes << ','
               << sample.device_local_memory_usage_bytes << ','
               << sample.gpu_arena_capacity_bytes << ','
               << sample.gpu_arena_used_bytes << ',' << sample.gpu_arena_free_bytes << ','
               << sample.gpu_arena_fragmentation << ',' << sample.pending_upload_bytes << ','
               << sample.uploaded_bytes_this_frame << '\n';
    }
    return output.str();
}

core::Status BenchmarkRecorder::write_json(const std::filesystem::path& path) const {
    return write_text_file(path, to_json());
}

core::Status BenchmarkRecorder::write_csv(const std::filesystem::path& path) const {
    return write_text_file(path, to_csv());
}

std::string format_benchmark_summary(const BenchmarkSummary& summary) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3) << summary.scene << ": n=" << summary.sample_count
           << " median=" << summary.median_frame_ms << "ms p95=" << summary.p95_frame_ms
           << "ms p99=" << summary.p99_frame_ms << "ms 1%low=" << summary.one_percent_low_fps
           << "fps 0.1%low=" << summary.point_one_percent_low_fps
           << "fps max=" << summary.maximum_frame_ms << "ms cpu=" << summary.mean_cpu_frame_ms
           << "ms gpu=";
    if (summary.gpu_sample_count == 0) {
        output << "unavailable";
    } else {
        output << summary.mean_gpu_frame_ms
               << "ms gpu_phases=" << summary.mean_gpu_opaque_terrain_ms << '/'
               << summary.mean_gpu_alpha_tested_terrain_ms << '/'
               << summary.mean_gpu_transparent_terrain_ms << "ms";
    }
    output << " gpu_upload=";
    if (summary.gpu_upload_sample_count == 0) {
        output << "unavailable";
    } else {
        output << summary.mean_gpu_upload_ms << "ms";
    }
    output << " relight=" << summary.median_voxel_relight_solve_ms << '/'
           << summary.p95_voxel_relight_solve_ms << "ms solve "
           << summary.median_voxel_relight_apply_ms << '/' << summary.p95_voxel_relight_apply_ms
           << "ms apply" << " relight_backlog=" << summary.maximum_voxel_relight_backlog_cells
           << " relight_budget_overruns=" << summary.final_voxel_relight_apply_budget_overruns;
    output << " fluid=" << summary.median_voxel_fluid_simulation_ms << '/'
           << summary.p95_voxel_fluid_simulation_ms << "ms simulate "
           << summary.median_voxel_fluid_apply_ms << '/' << summary.p95_voxel_fluid_apply_ms
           << "ms apply fluid_cells=" << summary.maximum_voxel_fluid_processed_cells << '/'
           << summary.maximum_voxel_fluid_active_cells
           << " fluid_budget_exhaustions=" << summary.final_voxel_fluid_budget_exhaustions;
    output << " particles=" << summary.maximum_particle_active
           << " update=" << summary.median_particle_update_ms << '/'
           << summary.p95_particle_update_ms << "ms present="
           << summary.median_particle_presentation_ms << '/'
           << summary.p95_particle_presentation_ms << "ms groups="
           << summary.maximum_particle_material_groups
           << " dropped=" << summary.final_particle_dropped;
    output << " sync=" << summary.mean_chunk_synchronization_ms
           << "ms cull=" << summary.mean_culling_ms << "ms build=" << summary.mean_command_build_ms
           << "ms record=" << summary.mean_command_recording_ms
           << "ms mesh=" << summary.mean_meshing_ms << "ms upload=" << summary.mean_upload_ms
           << "ms wait=" << summary.mean_gpu_wait_ms
           << "ms slowest_frame=" << summary.slowest_frame.frame_index;
    if (summary.budget.evaluated) {
        output << " budget=" << benchmark_budget_profile_name(summary.budget.profile) << ':'
               << (summary.budget.passed ? "pass" : "fail")
               << " violations=" << summary.budget.violations.size();
    }
    return output.str();
}

} // namespace heartstead::renderer::benchmark
