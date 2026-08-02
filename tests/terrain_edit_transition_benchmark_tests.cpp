#include "engine/renderer/benchmark/terrain_edit_transition_benchmark.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>

namespace {

namespace benchmark = heartstead::renderer::benchmark;

void test_config_validation() {
    benchmark::TerrainEditTransitionBenchmarkConfig config;
    assert(config.validate());

    auto invalid = config;
    invalid.world_radius_chunks = 1;
    assert(!invalid.validate());

    invalid = config;
    invalid.repetitions = 0;
    assert(!invalid.validate());

    invalid = config;
    invalid.update_interval_us = 0;
    assert(!invalid.validate());

    invalid = config;
    invalid.maximum_near_draw_p95_ms = 0.0;
    assert(!invalid.validate());

    invalid = config;
    invalid.far_rendering.lod_updates.mid_level_count = invalid.far_rendering.clipmap.level_count;
    assert(!invalid.validate());
}

void test_integrated_near_mid_far_edit_transition() {
    benchmark::TerrainEditTransitionBenchmarkConfig config;
    config.world_radius_chunks = 2;
    config.warmup_repetitions = 0;
    config.repetitions = 1;
    config.update_interval_us = 1'000;
    config.timeout_ms = 10'000;
    config.far_rendering.clipmap.patch_resolution = 4;
    auto report = benchmark::run_terrain_edit_transition_benchmark(config);
    if (!report) {
        std::cerr << report.error().code << ": " << report.error().message << '\n';
    }
    assert(report);
    assert(report.value().validate());
    assert(report.value().device.backend == "headless");
    assert(report.value().device.headless);
    assert(report.value().runs.size() == 1);
    assert(report.value().raw_samples.size() == 1);

    const auto& run = report.value().runs.front();
    assert(run.loaded_chunks == 25);
    assert(run.initial_near_resident_chunks > 0);
    assert(run.initial_far_resident_patches > 0);
    assert(run.initial_near_draw_commands > 0);
    assert(run.initial_far_draw_commands > 0);
    assert(run.supersession_near_coalesced_invalidations > 0);
    assert(run.supersession_far_coalesced_invalidations > 0);
    assert(run.supersession_far_stale_results > 0);
    assert(run.supersession_minimum_near_draw_commands > 0);
    assert(run.supersession_minimum_far_draw_commands > 0);
    assert(run.baseline_live_render_resources == run.final_live_render_resources);

    const auto& sample = report.value().raw_samples.front();
    assert(sample.near_draw_current_us > 0);
    assert(sample.mid_convergence_us >= sample.first_mid_publication_us);
    assert(sample.far_convergence_us >= sample.first_far_publication_us);
    assert(sample.full_convergence_us >= sample.near_draw_current_us);
    assert(sample.full_convergence_us >= sample.mid_convergence_us);
    assert(sample.full_convergence_us >= sample.far_convergence_us);
    assert(sample.instrumented_near_edit_to_visible_ms > 0.0);
    assert(sample.invalidated_far_patches ==
           sample.rebuilt_mid_patches + sample.rebuilt_far_patches);
    assert(sample.rebuilt_mid_patches > 0);
    assert(sample.rebuilt_far_patches > 0);
    assert(sample.minimum_near_draw_commands > 0);
    assert(sample.minimum_far_draw_commands > 0);
    assert(sample.maximum_far_pipeline_occupancy <=
           config.far_rendering.mesh_scheduler.maximum_concurrent_jobs);
    assert(sample.near_stale_mesh_results == 0);
    assert(sample.far_stale_results == 0);

    const auto summary = report.value().summary();
    assert(summary.run_count == 1);
    assert(summary.sample_count == 1);
    assert(summary.p95_near_draw_ms > 0.0);
    assert(summary.p95_mid_convergence_ms > 0.0);
    assert(summary.p95_far_convergence_ms > 0.0);
    assert(summary.p95_full_convergence_ms >= summary.p95_near_draw_ms);
    assert(report.value().gates_passed());

    const auto json = report.value().to_json();
    assert(json.contains("\"benchmark\": \"terrain_edit_transition\""));
    assert(json.contains("\"near\": \"first_exact_current_chunk_draw_command\""));
    assert(json.contains("\"mid\": \"all_invalidated_mid_patches_current\""));
    assert(json.contains("\"far\": \"all_invalidated_far_patches_current\""));
    assert(json.contains("\"raw_samples\""));
    assert(json.contains("\"supersession_far_stale_results\""));

    auto gated = report.value();
    gated.config.enforce_gates = true;
    gated.config.maximum_near_draw_p95_ms = summary.p95_near_draw_ms * 2.0;
    gated.config.maximum_mid_convergence_p95_ms = summary.p95_mid_convergence_ms * 2.0;
    gated.config.maximum_far_convergence_p95_ms = summary.p95_far_convergence_ms * 2.0;
    gated.config.maximum_full_convergence_p95_ms = summary.p95_full_convergence_ms * 2.0;
    gated.config.maximum_owner_update_ms = std::max(1.0, summary.maximum_owner_update_ms * 2.0);
    gated.config.maximum_upload_preparation_ms =
        std::max(1.0, summary.maximum_upload_preparation_ms * 2.0);
    assert(gated.gates_passed());
    gated.config.maximum_near_draw_p95_ms = summary.p95_near_draw_ms * 0.5;
    assert(!gated.gates_passed());
    assert(std::ranges::any_of(gated.summary().gates.violations, [](const auto& violation) {
        return violation.metric == "p95_near_draw_ms";
    }));

    auto corrupted = report.value();
    corrupted.raw_samples.front().minimum_far_draw_commands = 0;
    assert(!corrupted.validate());
}

} // namespace

int main() {
    test_config_validation();
    test_integrated_near_mid_far_edit_transition();
    return 0;
}
