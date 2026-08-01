#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heartstead::jobs {

struct JobIdTag;
using JobId = core::StrongU64Id<JobIdTag>;

enum class JobBackend {
    immediate,
    thread_pool,
};

enum class JobPriority {
    low,
    normal,
    high,
};

enum class JobState {
    queued,
    running,
    succeeded,
    failed,
    cancelled,
};

enum class JobCancellationReason : std::uint8_t {
    none,
    requested,
    superseded,
    shutdown,
};

struct JobBackendInfo {
    JobBackend backend = JobBackend::immediate;
    std::string_view name;
    bool available = false;
    std::string_view status;
};

class JobContext {
  public:
    JobContext(JobId job_id, std::string_view job_name, JobPriority job_priority,
               const std::atomic<JobCancellationReason>* cancellation) noexcept
        : id(job_id), name(job_name), priority(job_priority), cancellation_(cancellation) {}

    [[nodiscard]] bool cancellation_requested() const noexcept {
        return cancellation_reason() != JobCancellationReason::none;
    }

    [[nodiscard]] JobCancellationReason cancellation_reason() const noexcept {
        return cancellation_ == nullptr ? JobCancellationReason::none
                                        : cancellation_->load(std::memory_order_acquire);
    }

    JobId id;
    std::string_view name;
    JobPriority priority = JobPriority::normal;

  private:
    const std::atomic<JobCancellationReason>* cancellation_ = nullptr;
};

using JobFunction = std::function<core::Status(const JobContext&)>;

struct JobDesc {
    JobDesc() = default;

    JobDesc(std::string job_name, JobPriority job_priority, JobFunction job_work,
            std::string job_type = {}, std::uint32_t job_estimated_cost = 1)
        : name(std::move(job_name)), priority(job_priority), work(std::move(job_work)),
          type(std::move(job_type)), estimated_cost(job_estimated_cost) {}

    std::string name;
    JobPriority priority = JobPriority::normal;
    JobFunction work;
    std::string type;
    std::uint32_t estimated_cost = 1;
};

struct JobResult {
    JobId id;
    std::string name;
    std::string type;
    JobPriority priority = JobPriority::normal;
    std::uint32_t estimated_cost = 1;
    JobState state = JobState::queued;
    JobCancellationReason cancellation_reason = JobCancellationReason::none;
    std::uint64_t completion_order = 0;
    std::uint64_t enqueued_at_us = 0;
    std::uint64_t started_at_us = 0;
    std::uint64_t completed_at_us = 0;
    std::uint64_t queue_latency_us = 0;
    std::uint64_t execution_duration_us = 0;
    std::uint64_t total_latency_us = 0;
    std::string error_code;
    std::string error_message;
};

struct JobSystemDesc {
    JobSystemDesc() = default;

    JobSystemDesc(JobBackend job_backend, std::uint32_t job_worker_count = 1,
                  std::uint32_t completed_limit = 1024, std::uint64_t initial_job_id = 1,
                  std::uint32_t pending_limit = 4096)
        : backend(job_backend), worker_count(job_worker_count),
          max_completed_results(completed_limit), first_job_id(initial_job_id),
          max_pending_jobs(pending_limit) {}

    JobBackend backend = JobBackend::immediate;
    std::uint32_t worker_count = 1;
    std::uint32_t max_completed_results = 1024;
    std::uint64_t first_job_id = 1;
    std::uint32_t max_pending_jobs = 4096;
};

struct JobSystemStats {
    std::uint32_t pending_jobs = 0;
    std::uint32_t queued_jobs = 0;
    std::uint32_t running_jobs = 0;
    std::uint32_t publishing_jobs = 0;
    std::uint32_t completed_results = 0;
    std::uint32_t max_pending_jobs = 0;
    std::uint32_t max_completed_results = 0;
    std::uint64_t submitted_jobs = 0;
    std::uint64_t completed_jobs = 0;
    std::uint64_t rejected_submissions = 0;
    std::uint64_t cancellation_requests = 0;
    std::uint64_t cancelled_jobs = 0;
    std::uint64_t oldest_queued_job_age_us = 0;
    std::uint64_t maximum_queue_latency_us = 0;
};

class IJobSystem {
  public:
    virtual ~IJobSystem() = default;

    [[nodiscard]] virtual JobBackend backend() const noexcept = 0;
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t pending_count() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t submitted_count() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t completed_count() const noexcept = 0;
    [[nodiscard]] virtual JobSystemStats stats() const noexcept = 0;

    [[nodiscard]] virtual core::Result<JobId> submit(JobDesc desc) = 0;
    [[nodiscard]] virtual core::Status
    request_cancel(JobId id, JobCancellationReason reason = JobCancellationReason::requested) = 0;
    [[nodiscard]] virtual std::vector<JobResult> drain_completed() = 0;
};

[[nodiscard]] core::Result<std::unique_ptr<IJobSystem>> create_job_system(JobSystemDesc desc);

[[nodiscard]] core::Status validate_job_system_desc(const JobSystemDesc& desc);
[[nodiscard]] core::Status validate_job_desc(const JobDesc& desc);

[[nodiscard]] JobBackendInfo job_backend_info(JobBackend backend) noexcept;
[[nodiscard]] std::string_view job_backend_name(JobBackend backend) noexcept;
[[nodiscard]] std::string_view job_priority_name(JobPriority priority) noexcept;
[[nodiscard]] std::string_view job_state_name(JobState state) noexcept;
[[nodiscard]] std::string_view job_cancellation_reason_name(JobCancellationReason reason) noexcept;

} // namespace heartstead::jobs
