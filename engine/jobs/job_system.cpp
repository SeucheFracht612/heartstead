#include "engine/jobs/job_system.hpp"

#include "engine/jobs/thread_pool/thread_pool_backend.hpp"
#include "engine/profiling/profiler.hpp"

#include <exception>
#include <limits>
#include <utility>

namespace heartstead::jobs {

namespace {

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

    [[nodiscard]] core::Result<JobId> submit(JobDesc desc) override {
        HEARTSTEAD_PROFILE_ZONE_NAMED("jobs.immediate.execute");
        auto status = validate_job_desc(desc);
        if (!status) {
            return core::Result<JobId>::failure(status.error().code, status.error().message);
        }
        if (completed_results_.size() >= desc_.max_completed_results) {
            return core::Result<JobId>::failure("jobs.completed_queue_full",
                                                "completed job result queue is full");
        }

        auto id = next_job_id();
        if (!id) {
            return id;
        }
        ++submitted_count_;

        JobResult result;
        result.id = id.value();
        result.name = desc.name;
        result.priority = desc.priority;
        result.state = JobState::running;

        try {
            const JobContext context{id.value(), desc.name, desc.priority, false};
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

        result.completion_order = ++completed_count_;
        completed_results_.push(std::move(result));
        return id;
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
    std::queue<JobResult> completed_results_;
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

} // namespace heartstead::jobs
