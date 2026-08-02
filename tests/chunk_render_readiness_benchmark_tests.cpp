#include "engine/renderer/benchmark/chunk_render_readiness_benchmark.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>

namespace {

namespace benchmark = heartstead::renderer::benchmark;

void test_config_validation() {
    benchmark::ChunkRenderReadinessBenchmarkConfig config;
    assert(config.validate());

    auto invalid = config;
    invalid.horizontal_radius_chunks = 9;
    assert(!invalid.validate());

    invalid = config;
    invalid.repetitions = 0;
    assert(!invalid.validate());

    invalid = config;
    invalid.update_interval_us = 0;
    assert(!invalid.validate());

    invalid = config;
    invalid.maximum_synchronous_gpu_wait_ms = -1.0;
    assert(!invalid.validate());

    invalid = config;
    invalid.loading.worker_count = 0;
    assert(!invalid.validate());
}

void test_integrated_required_chunk_lifecycle() {
    benchmark::ChunkRenderReadinessBenchmarkConfig config;
    config.horizontal_radius_chunks = 1;
    config.warmup_repetitions = 0;
    config.repetitions = 1;
    config.update_interval_us = 1'000;
    config.timeout_ms = 5'000;
    auto report = benchmark::run_chunk_render_readiness_benchmark(config);
    assert(report);
    assert(report.value().validate());
    assert(report.value().device.backend == "headless");
    assert(report.value().device.headless);
    assert(report.value().runs.size() == 1);
    assert(report.value().raw_samples.size() == 5);

    const auto& run = report.value().runs.front();
    assert(run.desired_chunks == 5);
    assert(run.submitted_requests == 5);
    assert(run.published_requests == 5);
    assert(run.draw_eligible_chunks == 5);
    assert(run.failed_requests == 0);
    assert(run.failed_mesh_results == 0);
    assert(run.failed_uploads == 0);
    assert(run.pending_meshes == 0);
    assert(run.pending_uploads == 0);
    assert(run.current_mesh_stages == 5);
    assert(run.exact_resident_meshes == 5);
    assert(run.drawable_resident_meshes == 5);
    assert(run.uploaded_bytes > 0);
    assert(run.maximum_snapshot_cells_per_update <= config.rendering.max_snapshot_cells_per_frame);
    assert(run.maximum_uploaded_bytes_per_update <= config.rendering.max_bytes_uploaded_per_frame);
    assert(run.baseline_live_render_resources == run.final_live_render_resources);

    for (const auto& sample : report.value().raw_samples) {
        assert(sample.request_id != 0);
        assert(sample.load_generation != 0);
        assert(sample.mesh_request_revision != 0);
        assert(sample.interest_to_publication_us > 0);
        assert(sample.interest_to_mesh_resident_us >= sample.interest_to_publication_us);
        assert(sample.interest_to_draw_eligibility_us >= sample.interest_to_mesh_resident_us);
        assert(sample.publication_to_mesh_resident_us ==
               sample.interest_to_mesh_resident_us - sample.interest_to_publication_us);
        assert(sample.mesh_resident_to_draw_eligibility_us ==
               sample.interest_to_draw_eligibility_us - sample.interest_to_mesh_resident_us);
        assert(sample.draw_command_count > 0);
        assert(sample.vertex_count > 0);
        assert(sample.index_count > 0);
        assert(sample.resident_bytes > 0);
    }

    const auto summary = report.value().summary();
    assert(summary.run_count == 1);
    assert(summary.sample_count == 5);
    assert(summary.p95_interest_to_draw_eligibility_ms > 0.0);
    assert(summary.p95_interest_to_draw_eligibility_ms >= summary.p95_interest_to_publication_ms);
    assert(summary.maximum_gpu_wait_ms == 0.0);
    assert(report.value().gates_passed());

    const auto json = report.value().to_json();
    assert(json.contains("\"benchmark\": \"chunk_render_readiness\""));
    assert(json.contains("\"measurement_endpoint\": "
                         "\"first_exact_current_chunk_draw_command\""));
    assert(json.contains("\"excludes_gpu_execution_and_scanout\": true"));
    assert(json.contains("\"interest_to_draw_eligibility_us\""));
    assert(json.contains("\"maximum_upload_preparation_ms\""));

    auto gated = report.value();
    gated.config.enforce_gates = true;
    gated.config.maximum_draw_eligibility_p95_ms =
        summary.p95_interest_to_draw_eligibility_ms * 2.0;
    gated.config.maximum_upload_preparation_ms =
        std::max(1.0, summary.maximum_upload_preparation_ms * 2.0);
    gated.config.maximum_mesh_builds_per_publication = 100.0;
    assert(gated.gates_passed());
    gated.config.maximum_draw_eligibility_p95_ms =
        summary.p95_interest_to_draw_eligibility_ms * 0.5;
    assert(!gated.gates_passed());
    assert(std::ranges::any_of(gated.summary().gates.violations, [](const auto& violation) {
        return violation.metric == "p95_interest_to_draw_eligibility_ms";
    }));

    auto corrupted = report.value();
    corrupted.raw_samples.pop_back();
    assert(!corrupted.validate());
}

} // namespace

int main() {
    test_config_validation();
    test_integrated_required_chunk_lifecycle();
    return 0;
}
