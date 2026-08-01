#include "engine/jobs/job_system.hpp"

#include "engine/jobs/thread_pool/thread_pool_backend.hpp"
#include "engine/profiling/profiler.hpp"

#include <chrono>
#include <exception>
#include <limits>
#include <utility>

namespace heartstead::jobs {

namespace {

using JobClock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_microseconds(JobClock::time_point begin,
                                                 JobClock::time_point end) noexcept {
    const auto count = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    return count > 0 ? static_cast<std::uint64_t>(count) : 0;
}

[[nodiscard]] std::uint64_t timestamp_microseconds(JobClock::time_point origin,
                                                   JobClock::time_point point) noexcept {
    return elapsed_microseconds(origin, point) + 1;
}

template <typename T> [[nodiscard]] std::vector<T> drain_queue(std::queue<T>& queue) {
    std::vector<T> result;
    result.reserve(queue.size());
    while (!queue.empty()) {
        result.push_back(std::move(queue.front()));
        queue.pop();
    }
    return result;
}

class ImmediateJobSystem final : public IJobSystem {
  public:
    explicit ImmediateJobSystem(JobSystemDesc desc)
        : desc_(desc), next_job_id_(desc.first_job_id) {}

    [[nodiscard]] JobBackend backend() const noexcept override {
        return JobBackend::immediate;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return job_backend_name(JobBackend::immediate);
    }

    [[nodiscard]] std::uint32_t pending_count() const noexcept override {
        return 0;
    }

    [[nodiscard]] std::uint64_t submitted_count() const noexcept override {
        return submitted_count_;
    }

    [[nodiscard]] std::uint64_t completed_count() const noexcept override {
        return completed_count_;
    }

    [[nodiscard]] JobSystemStats stats() const noexcept override {
        JobSystemStats result;
        result.completed_results = static_cast<std::uint32_t>(completed_results_.size());
        result.max_pending_jobs = desc_.max_pending_jobs;
        result.max_completed_results = desc_.max_completed_results;
        result.submitted_jobs = submitted_count_;
        result.completed_jobs = completed_count_;
        result.rejected_submissions = rejected_submission_count_;
        result.cancellation_requests = cancellation_request_count_;
        return result;
    }

    [[nodiscard]] core::Result<JobId> submit(JobDesc desc) override {
        HEARTSTEAD_PROFILE_ZONE_NAMED("jobs.immediate.execute");
        auto status = validate_job_desc(desc);
        if (!status) {
            return core::Result<JobId>::failure(status.error().code, status.error().message);
        }
        if (completed_results_.size() >= desc_.max_completed_results) {
            ++rejected_submission_count_;
            return core::Result<JobId>::failure("jobs.completed_queue_full",
                                                "completed job result queue is full");
        }

        auto id = next_job_id();
        if (!id) {
            return id;
        }
        ++submitted_count_;

        const auto enqueued_at = JobClock::now();
        const auto started_at = JobClock::now();
        std::atomic cancellation{JobCancellationReason::none};

        JobResult result;
        result.id = id.value();
        result.name = desc.name;
        result.type = desc.type.empty() ? desc.name : std::move(desc.type);
        result.priority = desc.priority;
        result.estimated_cost = desc.estimated_cost;
        result.state = JobState::running;
        result.enqueued_at_us = timestamp_microseconds(origin_, enqueued_at);
        result.started_at_us = timestamp_microseconds(origin_, started_at);
        result.queue_latency_us = elapsed_microseconds(enqueued_at, started_at);

        try {
            const JobContext context{id.value(), desc.name, desc.priority, &cancellation};
            auto work_status = desc.work(context);
            if (work_status) {
                result.state = JobState::succeeded;
            } else {
                result.state = JobState::failed;
                result.error_code = work_status.error().code;
                result.error_message = work_status.error().message;
            }
        } catch (const std::exception& exception) {
            result.state = JobState::failed;
            result.error_code = "jobs.callback_exception";
            result.error_message =
                std::string("job callback threw an exception: ") + exception.what();
        } catch (...) {
            result.state = JobState::failed;
            result.error_code = "jobs.callback_exception";
            result.error_message = "job callback threw a non-standard exception";
        }

        const auto completed_at = JobClock::now();
        result.completed_at_us = timestamp_microseconds(origin_, completed_at);
        result.execution_duration_us = elapsed_microseconds(started_at, completed_at);
        result.total_latency_us = elapsed_microseconds(enqueued_at, completed_at);

        result.completion_order = ++completed_count_;
        completed_results_.push(std::move(result));
        return id;
    }

    [[nodiscard]] core::Status request_cancel(JobId id, JobCancellationReason reason) override {
        if (!id.is_valid()) {
            return core::Status::failure("jobs.invalid_id", "job id must be valid");
        }
        if (reason == JobCancellationReason::none) {
            return core::Status::failure("jobs.invalid_cancellation_reason",
                                         "job cancellation reason must not be none");
        }
        ++cancellation_request_count_;
        return core::Status::failure("jobs.not_active",
                                     "job is not queued or running in this job system");
    }

    [[nodiscard]] std::vector<JobResult> drain_completed() override {
        return drain_queue(completed_results_);
    }

  private:
    [[nodiscard]] core::Result<JobId> next_job_id() {
        if (next_job_id_ == 0) {
            return core::Result<JobId>::failure("jobs.id_range_exhausted",
                                                "job id range is exhausted");
        }
        const auto id = JobId::from_value(next_job_id_);
        next_job_id_ = next_job_id_ == std::numeric_limits<std::uint64_t>::max()
                           ? 0
                           : next_job_id_ + 1;
        return core::Result<JobId>::success(id);
    }

    JobSystemDesc desc_;
    std::uint64_t next_job_id_ = 1;
    std::uint64_t submitted_count_ = 0;
    std::uint64_t completed_count_ = 0;
    std::uint64_t rejected_submission_count_ = 0;
    std::uint64_t cancellation_request_count_ = 0;
    std::queue<JobResult> completed_results_;
    JobClock::time_point origin_ = JobClock::now();
};

} // namespace

core::Result<std::unique_ptr<IJobSystem>> create_job_system(JobSystemDesc desc) {
    auto status = validate_job_system_desc(desc);
    if (!status) {
        return core::Result<std::unique_ptr<IJobSystem>>::failure(status.error().code,
                                                                  status.error().message);
    }

    switch (desc.backend) {
    case JobBackend::immediate:
        return core::Result<std::unique_ptr<IJobSystem>>::success(
            std::make_unique<ImmediateJobSystem>(desc));
    case JobBackend::thread_pool:
        return thread_pool::create_job_system(desc);
    }

    return core::Result<std::unique_ptr<IJobSystem>>::failure("jobs.unknown_backend",
                                                              "unknown job backend");
}

core::Status validate_job_system_desc(const JobSystemDesc& desc) {
    if (desc.worker_count == 0) {
        return core::Status::failure("jobs.invalid_worker_count",
                                     "job system worker count must be non-zero");
    }
    if (desc.max_completed_results == 0) {
        return core::Status::failure("jobs.invalid_completed_limit",
                                     "job system completed result limit must be non-zero");
    }
    if (desc.max_pending_jobs == 0) {
        return core::Status::failure("jobs.invalid_pending_limit",
                                     "job system pending job limit must be non-zero");
    }
    if (desc.first_job_id == 0) {
        return core::Status::failure("jobs.invalid_first_id",
                                     "job system first job id must be non-zero");
    }
    return core::Status::ok();
}

core::Status validate_job_desc(const JobDesc& desc) {
    if (desc.name.empty()) {
        return core::Status::failure("jobs.missing_name", "job name is required");
    }
    if (!desc.work) {
        return core::Status::failure("jobs.missing_work", "job work function is required");
    }
    if (desc.estimated_cost == 0) {
        return core::Status::failure("jobs.invalid_estimated_cost",
                                     "job estimated cost must be non-zero");
    }
    return core::Status::ok();
}

JobBackendInfo job_backend_info(JobBackend backend) noexcept {
    switch (backend) {
    case JobBackend::immediate:
        return JobBackendInfo{
            JobBackend::immediate,
            job_backend_name(JobBackend::immediate),
            true,
            "available",
        };
    case JobBackend::thread_pool:
        return thread_pool::backend_info();
    }
    return JobBackendInfo{backend, "unknown", false, "unknown job backend"};
}

std::string_view job_backend_name(JobBackend backend) noexcept {
    switch (backend) {
    case JobBackend::immediate:
        return "immediate";
    case JobBackend::thread_pool:
        return "thread_pool";
    }
    return "unknown";
}

std::string_view job_priority_name(JobPriority priority) noexcept {
    switch (priority) {
    case JobPriority::low:
        return "low";
    case JobPriority::normal:
        return "normal";
    case JobPriority::high:
        return "high";
    }
    return "unknown";
}

std::string_view job_state_name(JobState state) noexcept {
    switch (state) {
    case JobState::queued:
        return "queued";
    case JobState::running:
        return "running";
    case JobState::succeeded:
        return "succeeded";
    case JobState::failed:
        return "failed";
    case JobState::cancelled:
        return "cancelled";
    }
    return "unknown";
}

std::string_view job_cancellation_reason_name(JobCancellationReason reason) noexcept {
    switch (reason) {
    case JobCancellationReason::none:
        return "none";
    case JobCancellationReason::requested:
        return "requested";
    case JobCancellationReason::superseded:
        return "superseded";
    case JobCancellationReason::shutdown:
        return "shutdown";
    }
    return "unknown";
}

} // namespace heartstead::jobs
