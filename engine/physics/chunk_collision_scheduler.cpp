#include "engine/physics/chunk_collision_scheduler.hpp"

#include "engine/profiling/cpu_timing.hpp"

#include <algorithm>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>

namespace heartstead::physics {

struct ChunkCollisionScheduler::SharedState {
    explicit SharedState(std::size_t maximum_cached_buffers)
        : maximum_cached_cell_buffers(maximum_cached_buffers) {}

    [[nodiscard]] std::vector<world::VoxelCell> acquire_cells(std::size_t minimum_capacity) {
        std::lock_guard lock(pool_mutex);
        auto best = cell_pool.end();
        for (auto candidate = cell_pool.begin(); candidate != cell_pool.end(); ++candidate) {
            if (candidate->capacity() >= minimum_capacity &&
                (best == cell_pool.end() || candidate->capacity() < best->capacity())) {
                best = candidate;
            }
        }
        if (best == cell_pool.end()) {
            std::vector<world::VoxelCell> result;
            result.reserve(minimum_capacity);
            return result;
        }
        auto result = std::move(*best);
        cell_pool.erase(best);
        return result;
    }

    void release_cells(std::vector<world::VoxelCell> cells) {
        cells.clear();
        std::lock_guard lock(pool_mutex);
        if (cell_pool.size() < maximum_cached_cell_buffers) {
            cell_pool.push_back(std::move(cells));
        }
    }

    void publish(ChunkCollisionResult result) {
        std::lock_guard lock(mailbox_mutex);
        mailbox.push_back(std::move(result));
    }

    [[nodiscard]] std::vector<ChunkCollisionResult> drain(std::size_t maximum_results) {
        std::vector<ChunkCollisionResult> results;
        std::lock_guard lock(mailbox_mutex);
        const auto count = std::min(maximum_results, mailbox.size());
        results.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            results.push_back(std::move(mailbox.front()));
            mailbox.pop_front();
        }
        return results;
    }

    [[nodiscard]] std::size_t mailbox_size() const noexcept {
        std::lock_guard lock(mailbox_mutex);
        return mailbox.size();
    }

    [[nodiscard]] std::pair<std::size_t, std::size_t> pool_stats() const noexcept {
        std::lock_guard lock(pool_mutex);
        std::size_t capacity = 0;
        for (const auto& cells : cell_pool) {
            capacity += cells.capacity();
        }
        return {cell_pool.size(), capacity};
    }

    std::size_t maximum_cached_cell_buffers = 0;
    mutable std::mutex pool_mutex;
    std::vector<std::vector<world::VoxelCell>> cell_pool;
    mutable std::mutex mailbox_mutex;
    std::deque<ChunkCollisionResult> mailbox;
};

core::Status ChunkCollisionSchedulerConfig::validate() const {
    if (worker_count == 0 || max_concurrent_jobs == 0 || max_completed_results == 0 ||
        max_cached_snapshot_buffers == 0) {
        return core::Status::failure(
            "chunk_collision.invalid_scheduler_config",
            "chunk collision scheduler limits and worker count must be nonzero");
    }
    if (max_concurrent_jobs < worker_count) {
        return core::Status::failure(
            "chunk_collision.invalid_scheduler_concurrency",
            "chunk collision concurrent-job limit must be at least the worker count");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<ChunkCollisionScheduler>>
ChunkCollisionScheduler::create(ChunkCollisionSchedulerConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<ChunkCollisionScheduler>>::failure(
            status.error().code, status.error().message);
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
    auto jobs = jobs::create_job_system(job_desc);
    if (!jobs) {
        return core::Result<std::unique_ptr<ChunkCollisionScheduler>>::failure(
            jobs.error().code, jobs.error().message);
    }
    auto shared_state = std::make_shared<SharedState>(config.max_cached_snapshot_buffers);
    return core::Result<std::unique_ptr<ChunkCollisionScheduler>>::success(
        std::unique_ptr<ChunkCollisionScheduler>(
            new ChunkCollisionScheduler(config, std::move(jobs).value(), std::move(shared_state))));
}

ChunkCollisionScheduler::ChunkCollisionScheduler(ChunkCollisionSchedulerConfig config,
                                                 std::unique_ptr<jobs::IJobSystem> jobs,
                                                 std::shared_ptr<SharedState> shared_state)
    : config_(config), jobs_(std::move(jobs)), shared_state_(std::move(shared_state)) {}

ChunkCollisionScheduler::~ChunkCollisionScheduler() {
    shutdown();
}

std::vector<world::VoxelCell>
ChunkCollisionScheduler::acquire_snapshot_cells(std::size_t minimum_capacity) {
    return shared_state_->acquire_cells(minimum_capacity);
}

core::Status ChunkCollisionScheduler::submit(ChunkCollisionRequest request) {
    if (jobs_ == nullptr) {
        shared_state_->release_cells(std::move(request.snapshot.cells));
        return core::Status::failure("chunk_collision.scheduler_stopped",
                                     "chunk collision scheduler is stopped");
    }
    auto snapshot_status = request.snapshot.validate();
    if (!snapshot_status || !request.stage_ticket.is_valid() ||
        request.stage_ticket.identity != request.snapshot.identity ||
        request.stage_ticket.stage != world::ChunkStage::collision ||
        request.collision_table == nullptr ||
        request.snapshot.collision_table_revision != request.collision_table->revision) {
        shared_state_->release_cells(std::move(request.snapshot.cells));
        return core::Status::failure("chunk_collision.invalid_request",
                                     "chunk collision request metadata is inconsistent");
    }
    if (active_jobs_.contains(request.snapshot.identity)) {
        shared_state_->release_cells(std::move(request.snapshot.cells));
        return core::Status::failure("chunk_collision.request_coalesced",
                                     "a collision cook is already active for this chunk identity");
    }
    if (!has_capacity()) {
        shared_state_->release_cells(std::move(request.snapshot.cells));
        return core::Status::failure("chunk_collision.scheduler_full",
                                     "chunk collision scheduler reached its concurrency budget");
    }

    const auto identity = request.snapshot.identity;
    const auto stage_revision = request.stage_ticket.revision;
    const auto center_revision = request.snapshot.content_revision;
    const auto table_revision = request.snapshot.collision_table_revision;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    auto shared_state = shared_state_;
    jobs::JobDesc job;
    job.name = "chunk_collision";
    job.type = "voxel.collision";
    job.priority = jobs::JobPriority::normal;
    job.estimated_cost = static_cast<std::uint32_t>(
        std::min(request.snapshot.cells.size(),
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    job.work = [request = std::move(request), cancellation,
                shared_state](const jobs::JobContext&) mutable {
        ChunkCollisionResult result;
        result.identity = request.snapshot.identity;
        result.stage_ticket = request.stage_ticket;
        result.center_revision = request.snapshot.content_revision;
        result.collision_table_revision = request.snapshot.collision_table_revision;
        if (cancellation->load(std::memory_order_acquire)) {
            result.state = ChunkCollisionResultState::cancelled;
            shared_state->release_cells(std::move(request.snapshot.cells));
            shared_state->publish(std::move(result));
            return core::Status::ok();
        }

        try {
            auto collision = [&]() {
                profiling::ScopedCpuTimer timer(result.cooking_ms);
                return world::cook_chunk_collision(request.snapshot, *request.collision_table);
            }();
            if (cancellation->load(std::memory_order_acquire)) {
                result.state = ChunkCollisionResultState::cancelled;
            } else if (collision) {
                result.state = ChunkCollisionResultState::succeeded;
                result.shape = std::move(collision).value();
            } else {
                result.state = ChunkCollisionResultState::failed;
                result.error_code = collision.error().code;
                result.error_message = collision.error().message;
            }
        } catch (const std::exception& exception) {
            result.state = ChunkCollisionResultState::failed;
            result.error_code = "chunk_collision.job_exception";
            result.error_message = exception.what();
        } catch (...) {
            result.state = ChunkCollisionResultState::failed;
            result.error_code = "chunk_collision.job_exception";
            result.error_message = "chunk collision worker threw an unknown exception";
        }
        shared_state->release_cells(std::move(request.snapshot.cells));
        shared_state->publish(std::move(result));
        return core::Status::ok();
    };

    auto submitted = jobs_->submit(std::move(job));
    if (!submitted) {
        return core::Status::failure(submitted.error().code, submitted.error().message);
    }
    active_jobs_.emplace(identity, ActiveJob{submitted.value(), stage_revision, center_revision,
                                             table_revision, std::move(cancellation)});
    ++stats_.submitted_jobs;
    refresh_stats();
    return core::Status::ok();
}

std::vector<ChunkCollisionResult>
ChunkCollisionScheduler::drain_completed(std::size_t maximum_results) {
    if (jobs_ != nullptr) {
        (void)jobs_->drain_completed();
    }
    auto results = shared_state_->drain(maximum_results);
    for (const auto& result : results) {
        const auto active = active_jobs_.find(result.identity);
        if (active != active_jobs_.end() &&
            active->second.stage_revision == result.stage_ticket.revision &&
            active->second.center_revision == result.center_revision &&
            active->second.collision_table_revision == result.collision_table_revision) {
            active_jobs_.erase(active);
        }
        ++stats_.completed_jobs;
        if (result.state == ChunkCollisionResultState::cancelled) {
            ++stats_.cancelled_jobs;
        } else if (result.state == ChunkCollisionResultState::failed) {
            ++stats_.failed_jobs;
        }
    }
    refresh_stats();
    return results;
}

void ChunkCollisionScheduler::cancel(world::ChunkIdentity identity) noexcept {
    const auto active = active_jobs_.find(identity);
    if (active != active_jobs_.end()) {
        active->second.cancellation->store(true, std::memory_order_release);
    }
}

void ChunkCollisionScheduler::cancel_all() noexcept {
    for (auto& [_, active] : active_jobs_) {
        active.cancellation->store(true, std::memory_order_release);
    }
}

void ChunkCollisionScheduler::shutdown() noexcept {
    if (jobs_ == nullptr) {
        return;
    }
    cancel_all();
    jobs_.reset();
    (void)shared_state_->drain(static_cast<std::size_t>(-1));
    active_jobs_.clear();
    refresh_stats();
}

bool ChunkCollisionScheduler::has_in_flight(world::ChunkIdentity identity) const noexcept {
    return active_jobs_.contains(identity);
}

std::optional<std::uint64_t>
ChunkCollisionScheduler::in_flight_stage_revision(world::ChunkIdentity identity) const noexcept {
    const auto active = active_jobs_.find(identity);
    return active == active_jobs_.end() ? std::nullopt
                                        : std::optional(active->second.stage_revision);
}

bool ChunkCollisionScheduler::has_capacity() const noexcept {
    return active_jobs_.size() < config_.max_concurrent_jobs;
}

const ChunkCollisionSchedulerStats& ChunkCollisionScheduler::stats() noexcept {
    refresh_stats();
    return stats_;
}

void ChunkCollisionScheduler::refresh_stats() noexcept {
    stats_.in_flight_jobs = active_jobs_.size();
    stats_.completed_mailbox_count = shared_state_->mailbox_size();
    const auto [buffers, capacity] = shared_state_->pool_stats();
    stats_.pooled_snapshot_buffers = buffers;
    stats_.pooled_snapshot_capacity_cells = capacity;
}

} // namespace heartstead::physics
