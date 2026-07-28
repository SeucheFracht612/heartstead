#include "engine/world/lighting/chunk_light_scheduler.hpp"

#include "engine/profiling/cpu_timing.hpp"

#include <algorithm>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>

namespace heartstead::world {

struct ChunkLightScheduler::SharedState {
    void publish(ChunkLightResult result) {
        std::lock_guard lock(mutex);
        mailbox.push_back(std::move(result));
    }

    [[nodiscard]] std::vector<ChunkLightResult> drain(std::size_t maximum_results) {
        std::vector<ChunkLightResult> results;
        std::lock_guard lock(mutex);
        const auto count = std::min(maximum_results, mailbox.size());
        results.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            results.push_back(std::move(mailbox.front()));
            mailbox.pop_front();
        }
        return results;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lock(mutex);
        return mailbox.size();
    }

    mutable std::mutex mutex;
    std::deque<ChunkLightResult> mailbox;
};

core::Status ChunkLightSchedulerConfig::validate() const {
    if (worker_count == 0 || max_completed_results == 0) {
        return core::Status::failure(
            "chunk_light.invalid_scheduler_config",
            "chunk light scheduler worker and completed-result limits must be nonzero");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<ChunkLightScheduler>>
ChunkLightScheduler::create(ChunkLightSchedulerConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<ChunkLightScheduler>>::failure(status.error().code,
                                                                           status.error().message);
    }
    jobs::JobSystemDesc jobs_desc;
    jobs_desc.backend = jobs::JobBackend::thread_pool;
    jobs_desc.worker_count = config.worker_count;
    jobs_desc.max_completed_results = static_cast<std::uint32_t>(
        std::min(config.max_completed_results,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    auto jobs = jobs::create_job_system(jobs_desc);
    if (!jobs) {
        return core::Result<std::unique_ptr<ChunkLightScheduler>>::failure(jobs.error().code,
                                                                           jobs.error().message);
    }
    auto shared_state = std::make_shared<SharedState>();
    return core::Result<std::unique_ptr<ChunkLightScheduler>>::success(
        std::unique_ptr<ChunkLightScheduler>(
            new ChunkLightScheduler(config, std::move(jobs).value(), std::move(shared_state))));
}

ChunkLightScheduler::ChunkLightScheduler(ChunkLightSchedulerConfig config,
                                         std::unique_ptr<jobs::IJobSystem> jobs,
                                         std::shared_ptr<SharedState> shared_state)
    : config_(config), jobs_(std::move(jobs)), shared_state_(std::move(shared_state)) {}

ChunkLightScheduler::~ChunkLightScheduler() {
    shutdown();
}

core::Status ChunkLightScheduler::submit(ChunkLightRequest request) {
    if (jobs_ == nullptr) {
        return core::Status::failure("chunk_light.scheduler_stopped",
                                     "chunk light scheduler is stopped");
    }
    auto snapshot_status = request.snapshot.validate();
    if (request.request_id == 0 || !snapshot_status || request.block_table == nullptr ||
        !request.block_table->validate()) {
        return core::Status::failure("chunk_light.invalid_request",
                                     "chunk light request metadata is inconsistent");
    }
    if (active_job_.has_value()) {
        return core::Status::failure("chunk_light.scheduler_full",
                                     "a voxel relight job is already active");
    }

    const auto request_id = request.request_id;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    auto shared_state = shared_state_;
    jobs::JobDesc job;
    job.name = "chunk_light";
    job.priority = jobs::JobPriority::normal;
    job.work = [request = std::move(request), cancellation,
                shared_state](const jobs::JobContext&) mutable {
        ChunkLightResult result;
        result.request_id = request.request_id;
        result.block_table_revision = request.block_table->revision;
        if (cancellation->load(std::memory_order_acquire)) {
            result.state = ChunkLightResultState::cancelled;
            shared_state->publish(std::move(result));
            return core::Status::ok();
        }
        try {
            auto light = [&]() {
                profiling::ScopedCpuTimer timer(result.solve_ms);
                return solve_voxel_light(request.snapshot, *request.block_table);
            }();
            if (cancellation->load(std::memory_order_acquire)) {
                result.state = ChunkLightResultState::cancelled;
            } else if (light) {
                result.state = ChunkLightResultState::succeeded;
                result.light = std::move(light).value();
            } else {
                result.state = ChunkLightResultState::failed;
                result.error_code = light.error().code;
                result.error_message = light.error().message;
            }
        } catch (const std::exception& exception) {
            result.state = ChunkLightResultState::failed;
            result.error_code = "chunk_light.job_exception";
            result.error_message = exception.what();
        } catch (...) {
            result.state = ChunkLightResultState::failed;
            result.error_code = "chunk_light.job_exception";
            result.error_message = "chunk light worker threw an unknown exception";
        }
        shared_state->publish(std::move(result));
        return core::Status::ok();
    };

    auto submitted = jobs_->submit(std::move(job));
    if (!submitted) {
        return core::Status::failure(submitted.error().code, submitted.error().message);
    }
    active_job_ = ActiveJob{submitted.value(), request_id, std::move(cancellation)};
    ++stats_.submitted_jobs;
    refresh_stats();
    return core::Status::ok();
}

std::vector<ChunkLightResult> ChunkLightScheduler::drain_completed(std::size_t maximum_results) {
    if (jobs_ != nullptr) {
        (void)jobs_->drain_completed();
    }
    auto results = shared_state_->drain(maximum_results);
    for (const auto& result : results) {
        if (active_job_.has_value() && active_job_->request_id == result.request_id) {
            active_job_.reset();
        }
        ++stats_.completed_jobs;
        if (result.state == ChunkLightResultState::cancelled) {
            ++stats_.cancelled_jobs;
        } else if (result.state == ChunkLightResultState::failed) {
            ++stats_.failed_jobs;
        }
    }
    refresh_stats();
    return results;
}

void ChunkLightScheduler::cancel() noexcept {
    if (active_job_.has_value()) {
        active_job_->cancellation->store(true, std::memory_order_release);
    }
}

void ChunkLightScheduler::shutdown() noexcept {
    if (jobs_ == nullptr) {
        return;
    }
    cancel();
    jobs_.reset();
    (void)shared_state_->drain(static_cast<std::size_t>(-1));
    active_job_.reset();
    refresh_stats();
}

bool ChunkLightScheduler::has_in_flight() const noexcept {
    return active_job_.has_value();
}

const ChunkLightSchedulerStats& ChunkLightScheduler::stats() noexcept {
    refresh_stats();
    return stats_;
}

void ChunkLightScheduler::refresh_stats() noexcept {
    stats_.in_flight = active_job_.has_value();
    stats_.completed_mailbox_count = shared_state_->size();
}

} // namespace heartstead::world
