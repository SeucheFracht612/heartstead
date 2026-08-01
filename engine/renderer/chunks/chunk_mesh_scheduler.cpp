#include "engine/renderer/chunks/chunk_mesh_scheduler.hpp"

#include "engine/profiling/cpu_timing.hpp"
#include "engine/world/meshing/greedy_chunk_mesher.hpp"

#include <algorithm>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace heartstead::renderer {

struct ChunkMeshScheduler::SharedState {
    SharedState(std::size_t maximum_cached_cell_buffers, std::size_t maximum_cached_mesh_buffers)
        : max_cached_cell_buffers(maximum_cached_cell_buffers),
          max_cached_mesh_buffers(maximum_cached_mesh_buffers) {}

    [[nodiscard]] std::vector<world::VoxelCell> acquire_cells(std::size_t minimum_capacity) {
        std::lock_guard lock(pool_mutex);
        auto best = cell_pool.end();
        for (auto candidate = cell_pool.begin(); candidate != cell_pool.end(); ++candidate) {
            if (candidate->capacity() >= minimum_capacity &&
                (best == cell_pool.end() || candidate->capacity() < best->capacity())) {
                best = candidate;
            }
        }
        if (best != cell_pool.end()) {
            auto result = std::move(*best);
            cell_pool.erase(best);
            return result;
        }
        std::vector<world::VoxelCell> result;
        result.reserve(minimum_capacity);
        return result;
    }

    void release_cells(std::vector<world::VoxelCell> cells) {
        cells.clear();
        std::lock_guard lock(pool_mutex);
        if (cell_pool.size() < max_cached_cell_buffers) {
            cell_pool.push_back(std::move(cells));
        }
    }

    [[nodiscard]] world::ChunkMesh acquire_mesh() {
        std::lock_guard lock(pool_mutex);
        if (mesh_pool.empty()) {
            return {};
        }
        auto result = std::move(mesh_pool.back());
        mesh_pool.pop_back();
        return result;
    }

    void release_mesh(world::ChunkMesh mesh) {
        mesh.vertices.clear();
        mesh.indices.clear();
        mesh.sections.clear();
        mesh.rich_instances.clear();
        mesh.face_count = 0;
        mesh.triangle_face_count = 0;
        std::lock_guard lock(pool_mutex);
        if (mesh_pool.size() < max_cached_mesh_buffers) {
            mesh_pool.push_back(std::move(mesh));
        }
    }

    void publish(ChunkMeshResult result) {
        std::lock_guard lock(mailbox_mutex);
        mailbox.push_back(std::move(result));
    }

    [[nodiscard]] std::vector<ChunkMeshResult> drain(std::size_t maximum_results) {
        std::vector<ChunkMeshResult> result;
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
        std::size_t cell_buffers = 0;
        std::size_t cell_capacity = 0;
        std::size_t mesh_buffers = 0;
        std::size_t mesh_vertex_capacity = 0;
        std::size_t mesh_index_capacity = 0;
        std::size_t mesh_section_capacity = 0;
    };

    [[nodiscard]] PoolStats pool_stats() const noexcept {
        std::lock_guard lock(pool_mutex);
        PoolStats result;
        result.cell_buffers = cell_pool.size();
        for (const auto& cells : cell_pool) {
            result.cell_capacity += cells.capacity();
        }
        result.mesh_buffers = mesh_pool.size();
        for (const auto& mesh : mesh_pool) {
            result.mesh_vertex_capacity += mesh.vertices.capacity();
            result.mesh_index_capacity += mesh.indices.capacity();
            result.mesh_section_capacity += mesh.sections.capacity();
        }
        return result;
    }

    std::size_t max_cached_cell_buffers = 0;
    std::size_t max_cached_mesh_buffers = 0;
    mutable std::mutex pool_mutex;
    std::vector<std::vector<world::VoxelCell>> cell_pool;
    std::vector<world::ChunkMesh> mesh_pool;
    mutable std::mutex mailbox_mutex;
    std::deque<ChunkMeshResult> mailbox;
};

namespace {

[[nodiscard]] jobs::JobPriority job_priority(const MeshPriority& priority) noexcept {
    if (priority.visible && priority.missing_resident_mesh) {
        return jobs::JobPriority::high;
    }
    if (priority.visible || priority.missing_resident_mesh) {
        return jobs::JobPriority::normal;
    }
    return jobs::JobPriority::low;
}

} // namespace

core::Status ChunkMeshSchedulerConfig::validate() const {
    if (worker_count == 0 || max_concurrent_jobs == 0 || max_completed_results == 0 ||
        max_cached_snapshot_buffers == 0 || max_cached_mesh_buffers == 0) {
        return core::Status::failure(
            "renderer.invalid_chunk_mesh_scheduler_config",
            "chunk mesh scheduler limits and worker count must be nonzero");
    }
    if (max_concurrent_jobs < worker_count) {
        return core::Status::failure(
            "renderer.invalid_chunk_mesh_concurrency",
            "chunk mesh concurrent-job limit must be at least the worker count");
    }
    if (meshing_mode != ChunkMeshingMode::reference && meshing_mode != ChunkMeshingMode::greedy) {
        return core::Status::failure("renderer.invalid_chunk_meshing_mode",
                                     "chunk meshing mode is not supported");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<ChunkMeshScheduler>>
ChunkMeshScheduler::create(ChunkMeshSchedulerConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<ChunkMeshScheduler>>::failure(status.error().code,
                                                                          status.error().message);
    }
    jobs::JobSystemDesc job_desc;
    job_desc.backend = jobs::JobBackend::thread_pool;
    job_desc.worker_count = config.worker_count;
    job_desc.max_completed_results = static_cast<std::uint32_t>(
        std::min(config.max_completed_results,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    job_desc.max_pending_jobs = static_cast<std::uint32_t>(
        std::min(config.max_concurrent_jobs,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    auto job_system = jobs::create_job_system(job_desc);
    if (!job_system) {
        return core::Result<std::unique_ptr<ChunkMeshScheduler>>::failure(
            job_system.error().code, job_system.error().message);
    }
    auto shared_state = std::make_shared<SharedState>(config.max_cached_snapshot_buffers,
                                                      config.max_cached_mesh_buffers);
    return core::Result<std::unique_ptr<ChunkMeshScheduler>>::success(
        std::unique_ptr<ChunkMeshScheduler>(new ChunkMeshScheduler(
            config, std::move(job_system).value(), std::move(shared_state))));
}

ChunkMeshScheduler::ChunkMeshScheduler(ChunkMeshSchedulerConfig config,
                                       std::unique_ptr<jobs::IJobSystem> jobs,
                                       std::shared_ptr<SharedState> shared_state)
    : config_(config), jobs_(std::move(jobs)), shared_state_(std::move(shared_state)) {}

ChunkMeshScheduler::~ChunkMeshScheduler() {
    shutdown();
}

std::vector<world::VoxelCell>
ChunkMeshScheduler::acquire_snapshot_cells(std::size_t minimum_capacity) {
    return shared_state_->acquire_cells(minimum_capacity);
}

core::Status ChunkMeshScheduler::submit(ChunkMeshRequest request) {
    if (jobs_ == nullptr) {
        shared_state_->release_cells(std::move(request.neighborhood.cells));
        return core::Status::failure("renderer.chunk_mesh_scheduler_stopped",
                                     "chunk mesh scheduler is stopped");
    }
    auto snapshot_status = request.neighborhood.validate();
    if (!snapshot_status) {
        shared_state_->release_cells(std::move(request.neighborhood.cells));
        return snapshot_status;
    }
    if (!request.identity.is_valid() || request.identity != request.neighborhood.center_identity ||
        request.center_revision == 0 ||
        request.center_revision != request.neighborhood.center_revision ||
        request.render_table == nullptr || request.block_render_table_revision == 0 ||
        request.block_render_table_revision != request.render_table->revision) {
        shared_state_->release_cells(std::move(request.neighborhood.cells));
        return core::Status::failure("renderer.invalid_chunk_mesh_request",
                                     "chunk mesh request metadata is inconsistent");
    }
    if (active_jobs_.contains(request.identity)) {
        shared_state_->release_cells(std::move(request.neighborhood.cells));
        return core::Status::failure("renderer.chunk_mesh_request_coalesced",
                                     "a chunk mesh job is already active for this identity");
    }
    if (!has_capacity()) {
        shared_state_->release_cells(std::move(request.neighborhood.cells));
        return core::Status::failure("renderer.chunk_mesh_scheduler_full",
                                     "chunk mesh scheduler reached its concurrency budget");
    }

    const auto identity = request.identity;
    const auto center_revision = request.center_revision;
    const auto table_revision = request.block_render_table_revision;
    const auto priority = request.priority;
    const auto meshing_mode = config_.meshing_mode;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    auto shared_state = shared_state_;
    jobs::JobDesc job;
    job.name = "chunk_mesh";
    job.type = "voxel.mesh";
    job.priority = job_priority(priority);
    job.estimated_cost = static_cast<std::uint32_t>(
        std::min(request.neighborhood.cells.size(),
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    job.work = [request = std::move(request), cancellation, meshing_mode,
                shared_state](const jobs::JobContext&) mutable {
        ChunkMeshResult result;
        result.identity = request.identity;
        result.center_revision = request.center_revision;
        result.block_render_table_revision = request.block_render_table_revision;
        result.priority = request.priority;
        if (cancellation->load(std::memory_order_acquire)) {
            result.state = ChunkMeshResultState::cancelled;
            result.dependency_revisions = std::move(request.neighborhood.dependencies);
            shared_state->release_cells(std::move(request.neighborhood.cells));
            shared_state->publish(std::move(result));
            return core::Status::ok();
        }

        try {
            auto mesh = [&]() {
                profiling::ScopedCpuTimer timer(result.meshing_ms);
                if (meshing_mode == ChunkMeshingMode::reference) {
                    return world::ChunkMesher::build_surface_mesh(request.neighborhood,
                                                                  *request.render_table);
                }
                auto reusable_mesh = shared_state->acquire_mesh();
                return world::GreedyChunkMesher::build_surface_mesh(
                    request.neighborhood, *request.render_table, std::move(reusable_mesh));
            }();
            if (mesh) {
                result.state = ChunkMeshResultState::succeeded;
                result.mesh = std::move(mesh).value();
            } else {
                result.state = ChunkMeshResultState::failed;
                result.error_code = mesh.error().code;
                result.error_message = mesh.error().message;
            }
        } catch (const std::exception& exception) {
            result.state = ChunkMeshResultState::failed;
            result.error_code = "renderer.chunk_mesh_job_exception";
            result.error_message = exception.what();
        } catch (...) {
            result.state = ChunkMeshResultState::failed;
            result.error_code = "renderer.chunk_mesh_job_exception";
            result.error_message = "chunk mesh worker threw an unknown exception";
        }
        result.dependency_revisions = std::move(request.neighborhood.dependencies);
        shared_state->release_cells(std::move(request.neighborhood.cells));
        shared_state->publish(std::move(result));
        return core::Status::ok();
    };

    auto submitted = jobs_->submit(std::move(job));
    if (!submitted) {
        // The moved request is owned by the rejected JobDesc and releases normally here. It was
        // never handed to a worker, so no active record is installed.
        return core::Status::failure(submitted.error().code, submitted.error().message);
    }
    active_jobs_.emplace(identity, ActiveJob{submitted.value(), center_revision, table_revision,
                                             std::move(cancellation)});
    ++stats_.submitted_jobs;
    refresh_stats();
    return core::Status::ok();
}

std::vector<ChunkMeshResult> ChunkMeshScheduler::drain_completed(std::size_t maximum_results) {
    if (jobs_ != nullptr) {
        (void)jobs_->drain_completed();
    }
    auto results = shared_state_->drain(maximum_results);
    for (const auto& result : results) {
        const auto active = active_jobs_.find(result.identity);
        if (active != active_jobs_.end() &&
            active->second.center_revision == result.center_revision &&
            active->second.render_table_revision == result.block_render_table_revision) {
            active_jobs_.erase(active);
        }
        ++stats_.completed_jobs;
        if (result.state == ChunkMeshResultState::cancelled) {
            ++stats_.cancelled_jobs;
        } else if (result.state == ChunkMeshResultState::failed) {
            ++stats_.failed_jobs;
        }
    }
    refresh_stats();
    return results;
}

void ChunkMeshScheduler::recycle_mesh(world::ChunkMesh mesh) noexcept {
    shared_state_->release_mesh(std::move(mesh));
    refresh_stats();
}

void ChunkMeshScheduler::cancel(world::ChunkIdentity identity) noexcept {
    const auto active = active_jobs_.find(identity);
    if (active != active_jobs_.end()) {
        active->second.cancellation->store(true, std::memory_order_release);
    }
}

void ChunkMeshScheduler::cancel_all() noexcept {
    for (auto& [_, active] : active_jobs_) {
        active.cancellation->store(true, std::memory_order_release);
    }
}

void ChunkMeshScheduler::shutdown() noexcept {
    if (jobs_ == nullptr) {
        return;
    }
    cancel_all();
    jobs_.reset();
    (void)shared_state_->drain(static_cast<std::size_t>(-1));
    active_jobs_.clear();
    refresh_stats();
}

bool ChunkMeshScheduler::has_in_flight(world::ChunkIdentity identity) const noexcept {
    return active_jobs_.contains(identity);
}

std::optional<std::uint64_t>
ChunkMeshScheduler::in_flight_revision(world::ChunkIdentity identity) const noexcept {
    const auto active = active_jobs_.find(identity);
    if (active == active_jobs_.end()) {
        return std::nullopt;
    }
    return active->second.center_revision;
}

bool ChunkMeshScheduler::has_capacity() const noexcept {
    return active_jobs_.size() < config_.max_concurrent_jobs;
}

const ChunkMeshSchedulerStats& ChunkMeshScheduler::stats() noexcept {
    refresh_stats();
    return stats_;
}

void ChunkMeshScheduler::refresh_stats() noexcept {
    stats_.in_flight_jobs = active_jobs_.size();
    stats_.completed_mailbox_count = shared_state_->mailbox_size();
    const auto pool = shared_state_->pool_stats();
    stats_.pooled_snapshot_buffers = pool.cell_buffers;
    stats_.pooled_snapshot_capacity_cells = pool.cell_capacity;
    stats_.pooled_mesh_buffers = pool.mesh_buffers;
    stats_.pooled_mesh_vertex_capacity = pool.mesh_vertex_capacity;
    stats_.pooled_mesh_index_capacity = pool.mesh_index_capacity;
    stats_.pooled_mesh_section_capacity = pool.mesh_section_capacity;
}

} // namespace heartstead::renderer
