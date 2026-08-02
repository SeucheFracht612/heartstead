#include "engine/world/benchmark/voxel_response_benchmark.hpp"
#include "engine/world/voxels/voxel_chunk.hpp"

#include <cassert>
#include <cmath>
#include <string>

namespace {

using namespace heartstead;

void test_configuration_rejects_unbounded_or_censored_runs() {
    world::benchmark::VoxelResponseBenchmarkConfig config;
    config.physics_backend = static_cast<physics::PhysicsBackend>(255);
    assert(!config.validate());

    config = {};
    config.horizontal_radius_chunks = 5;
    assert(!config.validate());

    config = {};
    config.repetitions = 0;
    assert(!config.validate());

    config = {};
    config.update_interval_us = 0;
    assert(!config.validate());

    config = {};
    config.maximum_collision_p95_ms = 0.0;
    assert(!config.validate());

    config = {};
    config.lighting.max_snapshot_cells_per_update = 0;
    assert(!config.validate());
}

void test_smoke_run_retains_exact_response_samples_and_gate_evidence() {
    world::benchmark::VoxelResponseBenchmarkConfig config;
    config.horizontal_radius_chunks = 0;
    config.warmup_repetitions = 0;
    config.repetitions = 2;
    config.update_interval_us = 1;
    config.timeout_ms = 5'000;
    config.lighting.max_snapshot_cells_per_update = world::VoxelChunk::total_cells;
    config.enforce_gates = true;
    config.maximum_collision_p95_ms = 10'000.0;
    config.maximum_relight_p95_ms = 10'000.0;

    auto executed = world::benchmark::run_voxel_response_benchmark(config);
    assert(executed);
    const auto& report = executed.value();
    assert(report.validate());
    assert(report.run.chunk_count == 1);
    assert(report.run.collision_response_completions == config.repetitions);
    assert(report.run.relight_response_completions == config.repetitions);
    assert(report.run.current_collision_stages == 1);
    assert(report.run.current_lighting_stages == 1);
    assert(report.raw_samples.size() == config.repetitions);
    for (std::size_t index = 0; index < report.raw_samples.size(); ++index) {
        const auto& sample = report.raw_samples[index];
        assert(sample.repetition == index);
        assert(sample.owner_updates > 0);
        assert(std::isfinite(sample.collision_response_ms));
        assert(sample.collision_response_ms > 0.0);
        assert(std::isfinite(sample.relight_convergence_ms));
        assert(sample.relight_convergence_ms > 0.0);
        assert(std::isfinite(sample.maximum_owner_update_ms));
        assert(sample.maximum_owner_update_ms >= 0.0);
        assert(sample.snapshot_cells_copied >= world::VoxelChunk::total_cells);
    }

    const auto summary = report.summary();
    assert(summary.sample_count == config.repetitions);
    assert(summary.p95_collision_response_ms > 0.0);
    assert(summary.p95_relight_convergence_ms > 0.0);
    assert(summary.gates.evaluated);
    assert(summary.gates.passed);
    assert(report.gates_passed());

    const auto json = report.to_json();
    assert(json.find("\"benchmark\": \"voxel_response\"") != std::string::npos);
    assert(json.find("\"physics_backend\": \"headless\"") != std::string::npos);
    assert(json.find("\"raw_samples\"") != std::string::npos);
    assert(json.find("\"p95_collision_response_ms\"") != std::string::npos);
    assert(json.find("\"p95_relight_convergence_ms\"") != std::string::npos);

    auto gate_failure = report;
    gate_failure.config.maximum_collision_p95_ms = 1.0e-9;
    gate_failure.config.maximum_relight_p95_ms = 1.0e-9;
    assert(gate_failure.validate());
    assert(!gate_failure.gates_passed());
    assert(gate_failure.summary().gates.violations.size() == 2);

    auto incomplete = report;
    incomplete.raw_samples.pop_back();
    assert(!incomplete.validate());
}

void test_available_jolt_backend_uses_the_same_response_contract() {
    if (!physics::physics_backend_info(physics::PhysicsBackend::jolt).available) {
        return;
    }
    world::benchmark::VoxelResponseBenchmarkConfig config;
    config.physics_backend = physics::PhysicsBackend::jolt;
    config.horizontal_radius_chunks = 0;
    config.warmup_repetitions = 0;
    config.repetitions = 1;
    config.update_interval_us = 1;
    config.timeout_ms = 5'000;
    config.lighting.max_snapshot_cells_per_update = world::VoxelChunk::total_cells;

    auto executed = world::benchmark::run_voxel_response_benchmark(config);
    assert(executed);
    assert(executed.value().validate());
    assert(executed.value().raw_samples.size() == 1);
    assert(executed.value().to_json().find("\"physics_backend\": \"jolt\"") != std::string::npos);
}

} // namespace

int main() {
    test_configuration_rejects_unbounded_or_censored_runs();
    test_smoke_run_retains_exact_response_samples_and_gate_evidence();
    test_available_jolt_backend_uses_the_same_response_contract();
    return 0;
}
