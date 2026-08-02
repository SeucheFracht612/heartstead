#include "engine/renderer/terrain/far_terrain_mesh_scheduler.hpp"

#include "engine/profiling/cpu_timing.hpp"

#include <algorithm>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>

namespace heartstead::renderer {

struct FarTerrainMeshScheduler::SharedState {
    SharedState(std::size_t maximum_surface_buffers, std::size_t maximum_mesh_buffers)
        : maximum_cached_surface_buffers(maximum_surface_buffers),
          maximum_cached_mesh_buffers(maximum_mesh_buffers) {}

    [[nodiscard]] std::vector<FarTerrainSurfaceSample>
    acquire_surface_samples(std::size_t minimum_capacity) {
        std::lock_guard lock(pool_mutex);
        auto best = surface_pool.end();
        for (auto candidate = surface_pool.begin(); candidate != surface_pool.end(); ++candidate) {
            if (candidate->capacity() >= minimum_capacity &&
                (best == surface_pool.end() || candidate->capacity() < best->capacity())) {
                best = candidate;
            }
        }
        if (best != surface_pool.end()) {
            auto result = std::move(*best);
            surface_pool.erase(best);
            return result;
        }
        std::vector<FarTerrainSurfaceSample> result;
        result.reserve(minimum_capacity);
        return result;
    }

    void release_surface_samples(std::vector<FarTerrainSurfaceSample> samples) {
        samples.clear();
        std::lock_guard lock(pool_mutex);
        if (surface_pool.size() < maximum_cached_surface_buffers) {
            surface_pool.push_back(std::move(samples));
        }
    }

    [[nodiscard]] FarTerrainPatchMesh acquire_mesh() {
        std::lock_guard lock(pool_mutex);
        if (mesh_pool.empty()) {
            return {};
        }
        auto result = std::move(mesh_pool.back());
        mesh_pool.pop_back();
        return result;
    }

    void release_mesh(FarTerrainPatchMesh mesh) {
        mesh.vertices.clear();
        mesh.indices.clear();
        mesh.local_bounds = {};
        std::lock_guard lock(pool_mutex);
        if (mesh_pool.size() < maximum_cached_mesh_buffers) {
            mesh_pool.push_back(std::move(mesh));
        }
    }

    void publish(FarTerrainMeshResult result) {
        std::lock_guard lock(mailbox_mutex);
        mailbox.push_back(std::move(result));
    }

    [[nodiscard]] std::vector<FarTerrainMeshResult> drain(std::size_t maximum_results) {
        std::vector<FarTerrainMeshResult> result;
        std::lock_guard lock(mailbox_mutex);
        const auto count = std::min(maximum_results, mailbox.size());
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(std::move(mailbox.front()));
            mailbox.pop_front();
        }
        return result;
    }

    [[nodiscard]] std::size_t mailbox_size() const noexcept {
        std::lock_guard lock(mailbox_mutex);
        return mailbox.size();
    }

    struct PoolStats {
        std::size_t surface_buffers = 0;
        std::size_t surface_sample_capacity = 0;
        std::size_t mesh_buffers = 0;
        std::size_t mesh_vertex_capacity = 0;
        std::size_t mesh_index_capacity = 0;
    };

    [[nodiscard]] PoolStats pool_stats() const noexcept {
        std::lock_guard lock(pool_mutex);
        PoolStats result;
        result.surface_buffers = surface_pool.size();
        for (const auto& samples : surface_pool) {
            result.surface_sample_capacity += samples.capacity();
        }
        result.mesh_buffers = mesh_pool.size();
        for (const auto& mesh : mesh_pool) {
            result.mesh_vertex_capacity += mesh.vertices.capacity();
            result.mesh_index_capacity += mesh.indices.capacity();
        }
        return result;
    }

    std::size_t maximum_cached_surface_buffers = 0;
    std::size_t maximum_cached_mesh_buffers = 0;
    mutable std::mutex pool_mutex;
    std::vector<std::vector<FarTerrainSurfaceSample>> surface_pool;
    std::vector<FarTerrainPatchMesh> mesh_pool;
    mutable std::mutex mailbox_mutex;
    std::deque<FarTerrainMeshResult> mailbox;
};

core::Status FarTerrainMeshSchedulerConfig::validate() const {
    if (worker_count == 0 || maximum_concurrent_jobs == 0 || maximum_completed_results == 0 ||
        maximum_cached_surface_buffers == 0 || maximum_cached_mesh_buffers == 0) {
        return core::Status::failure(
            "renderer.invalid_far_terrain_mesh_scheduler_config",
            "far-terrain mesh scheduler limits and worker count must be nonzero");
    }
    if (maximum_concurrent_jobs < worker_count) {
        return core::Status::failure(
            "renderer.invalid_far_terrain_mesh_scheduler_concurrency",
            "far-terrain concurrent-job limit must be at least the worker count");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<FarTerrainMeshScheduler>>
FarTerrainMeshScheduler::create(FarTerrainClipmap clipmap, FarTerrainMeshSchedulerConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<FarTerrainMeshScheduler>>::failure(
            status.error().code, status.error().message);
    }
    jobs::JobSystemDesc job_desc;
    job_desc.backend = jobs::JobBackend::thread_pool;
    job_desc.worker_count = config.worker_count;
    job_desc.max_completed_results = static_cast<std::uint32_t>(
        std::min(config.maximum_completed_results,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    job_desc.max_pending_jobs = static_cast<std::uint32_t>(
        std::min(config.maximum_concurrent_jobs,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    auto job_system = jobs::create_job_system(job_desc);
    if (!job_system) {
        return core::Result<std::unique_ptr<FarTerrainMeshScheduler>>::failure(
            job_system.error().code, job_system.error().message);
    }
    auto shared_state = std::make_shared<SharedState>(config.maximum_cached_surface_buffers,
                                                      config.maximum_cached_mesh_buffers);
    auto shared_clipmap = std::make_shared<const FarTerrainClipmap>(std::move(clipmap));
    return core::Result<std::unique_ptr<FarTerrainMeshScheduler>>::success(
        std::unique_ptr<FarTerrainMeshScheduler>(
            new FarTerrainMeshScheduler(config, std::move(job_system).value(),
                                        std::move(shared_clipmap), std::move(shared_state))));
}

FarTerrainMeshScheduler::FarTerrainMeshScheduler(FarTerrainMeshSchedulerConfig config,
                                                 std::unique_ptr<jobs::IJobSystem> jobs,
                                                 std::shared_ptr<const FarTerrainClipmap> clipmap,
                                                 std::shared_ptr<SharedState> shared_state)
    : config_(config), jobs_(std::move(jobs)), clipmap_(std::move(clipmap)),
      shared_state_(std::move(shared_state)) {}

FarTerrainMeshScheduler::~FarTerrainMeshScheduler() {
    shutdown();
}

std::vector<FarTerrainSurfaceSample>
FarTerrainMeshScheduler::acquire_surface_samples(std::size_t minimum_capacity) {
    return shared_state_->acquire_surface_samples(minimum_capacity);
}

core::Status FarTerrainMeshScheduler::submit(FarTerrainMeshRequest request) {
    if (jobs_ == nullptr) {
        shared_state_->release_surface_samples(std::move(request.surface.samples));
        return core::Status::failure("renderer.far_terrain_mesh_scheduler_stopped",
                                     "far-terrain mesh scheduler is stopped");
    }
    auto surface_status = request.surface.validate_for(request.update.patch);
    if (!surface_status || request.update.request_revision == 0 ||
        request.update.patch.key != request.surface.key) {
        shared_state_->release_surface_samples(std::move(request.surface.samples));
        return core::Status::failure("renderer.invalid_far_terrain_mesh_request",
                                     "far-terrain mesh request metadata is inconsistent");
    }
    const auto key = request.update.patch.key;
    if (active_jobs_.contains(key)) {
        shared_state_->release_surface_samples(std::move(request.surface.samples));
        return core::Status::failure("renderer.far_terrain_mesh_request_coalesced",
                                     "a far-terrain mesh job is already active for this patch");
    }
    if (!has_capacity()) {
        shared_state_->release_surface_samples(std::move(request.surface.samples));
        return core::Status::failure("renderer.far_terrain_mesh_scheduler_full",
                                     "far-terrain mesh scheduler reached its concurrency budget");
    }

    const auto request_revision = request.update.request_revision;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    auto shared_state = shared_state_;
    auto clipmap = clipmap_;
    jobs::JobDesc job;
    job.name = "far_terrain_mesh";
    job.type = "terrain.mesh";
    job.priority = request.update.band == FarTerrainLodBand::mid ? jobs::JobPriority::high
                                                                 : jobs::JobPriority::normal;
    job.estimated_cost = static_cast<std::uint32_t>(
        std::min(request.surface.samples.size(),
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    job.work = [request = std::move(request), cancellation, shared_state,
                clipmap](const jobs::JobContext&) mutable {
        FarTerrainMeshResult result;
        result.update = request.update;
        const auto publish_cancelled = [&]() {
            result.state = FarTerrainMeshResultState::cancelled;
            shared_state->release_surface_samples(std::move(request.surface.samples));
            shared_state->publish(std::move(result));
        };
        if (cancellation->load(std::memory_order_acquire)) {
            publish_cancelled();
            return core::Status::ok();
        }

        try {
            auto reusable_mesh = shared_state->acquire_mesh();
            auto mesh = [&]() {
                profiling::ScopedCpuTimer timer(result.meshing_ms);
                return clipmap->build_patch_mesh(request.update.patch, request.surface,
                                                 std::move(reusable_mesh));
            }();
            if (cancellation->load(std::memory_order_acquire)) {
                if (mesh) {
                    shared_state->release_mesh(std::move(mesh).value());
                }
                result.state = FarTerrainMeshResultState::cancelled;
            } else if (mesh) {
                result.state = FarTerrainMeshResultState::succeeded;
                result.mesh = std::move(mesh).value();
            } else {
                result.state = FarTerrainMeshResultState::failed;
                result.error_code = mesh.error().code;
                result.error_message = mesh.error().message;
            }
        } catch (const std::exception& exception) {
            result.state = FarTerrainMeshResultState::failed;
            result.error_code = "renderer.far_terrain_mesh_job_exception";
            result.error_message = exception.what();
        } catch (...) {
            result.state = FarTerrainMeshResultState::failed;
            result.error_code = "renderer.far_terrain_mesh_job_exception";
            result.error_message = "far-terrain mesh worker threw an unknown exception";
        }
        shared_state->release_surface_samples(std::move(request.surface.samples));
        shared_state->publish(std::move(result));
        return core::Status::ok();
    };

    auto submitted = jobs_->submit(std::move(job));
    if (!submitted) {
        return core::Status::failure(submitted.error().code, submitted.error().message);
    }
    active_jobs_.emplace(key,
                         ActiveJob{submitted.value(), request_revision, std::move(cancellation)});
    ++stats_.submitted_jobs;
    refresh_stats();
    return core::Status::ok();
}

std::vector<FarTerrainMeshResult>
FarTerrainMeshScheduler::drain_completed(std::size_t maximum_results) {
    if (jobs_ != nullptr) {
        (void)jobs_->drain_completed();
    }
    auto results = shared_state_->drain(maximum_results);
    for (const auto& result : results) {
        const auto active = active_jobs_.find(result.update.patch.key);
        if (active != active_jobs_.end() &&
            active->second.request_revision == result.update.request_revision) {
            active_jobs_.erase(active);
        }
        ++stats_.completed_jobs;
        if (result.state == FarTerrainMeshResultState::cancelled) {
            ++stats_.cancelled_jobs;
        } else if (result.state == FarTerrainMeshResultState::failed) {
            ++stats_.failed_jobs;
        }
    }
    refresh_stats();
    return results;
}

void FarTerrainMeshScheduler::recycle_mesh(FarTerrainPatchMesh mesh) noexcept {
    shared_state_->release_mesh(std::move(mesh));
    refresh_stats();
}

void FarTerrainMeshScheduler::cancel(const FarTerrainPatchKey& key) noexcept {
    const auto active = active_jobs_.find(key);
    if (active != active_jobs_.end()) {
        active->second.cancellation->store(true, std::memory_order_release);
    }
}

void FarTerrainMeshScheduler::cancel_all() noexcept {
    for (auto& [key, active] : active_jobs_) {
        static_cast<void>(key);
        active.cancellation->store(true, std::memory_order_release);
    }
}

void FarTerrainMeshScheduler::shutdown() noexcept {
    if (jobs_ == nullptr) {
        return;
    }
    cancel_all();
    jobs_.reset();
    auto discarded = shared_state_->drain(static_cast<std::size_t>(-1));
    for (auto& result : discarded) {
        if (result.mesh.has_value()) {
            shared_state_->release_mesh(std::move(*result.mesh));
        }
    }
    active_jobs_.clear();
    refresh_stats();
}

bool FarTerrainMeshScheduler::has_in_flight(const FarTerrainPatchKey& key) const noexcept {
    return active_jobs_.contains(key);
}

std::optional<std::uint64_t>
FarTerrainMeshScheduler::in_flight_request_revision(const FarTerrainPatchKey& key) const noexcept {
    const auto active = active_jobs_.find(key);
    return active == active_jobs_.end()
               ? std::nullopt
               : std::optional<std::uint64_t>{active->second.request_revision};
}

bool FarTerrainMeshScheduler::has_capacity() const noexcept {
    return active_jobs_.size() < config_.maximum_concurrent_jobs;
}

const FarTerrainMeshSchedulerStats& FarTerrainMeshScheduler::stats() noexcept {
    refresh_stats();
    return stats_;
}

void FarTerrainMeshScheduler::refresh_stats() noexcept {
    stats_.in_flight_jobs = active_jobs_.size();
    stats_.completed_mailbox_count = shared_state_->mailbox_size();
    const auto pool = shared_state_->pool_stats();
    stats_.pooled_surface_buffers = pool.surface_buffers;
    stats_.pooled_surface_sample_capacity = pool.surface_sample_capacity;
    stats_.pooled_mesh_buffers = pool.mesh_buffers;
    stats_.pooled_mesh_vertex_capacity = pool.mesh_vertex_capacity;
    stats_.pooled_mesh_index_capacity = pool.mesh_index_capacity;
}

} // namespace heartstead::renderer
