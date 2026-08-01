#include "engine/jobs/thread_pool/thread_pool_backend.hpp"

#include "engine/profiling/profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heartstead::jobs::thread_pool {

namespace {

using JobClock = std::chrono::steady_clock;

constexpr std::uint64_t priority_aging_dispatches = 8;

struct QueuedJob {
    JobId id;
    std::string name;
    std::string type;
    JobPriority priority = JobPriority::normal;
    std::uint32_t estimated_cost = 1;
    JobFunction work;
    std::shared_ptr<std::atomic<JobCancellationReason>> cancellation;
    JobClock::time_point enqueued_at;
    std::uint64_t enqueue_sequence = 0;
    std::uint64_t enqueued_dispatch_count = 0;
};

[[nodiscard]] int priority_rank(JobPriority priority) noexcept {
    switch (priority) {
    case JobPriority::low:
        return 0;
    case JobPriority::normal:
        return 1;
    case JobPriority::high:
        return 2;
    }
    return 0;
}

[[nodiscard]] std::uint64_t elapsed_microseconds(JobClock::time_point begin,
                                                 JobClock::time_point end) noexcept {
    const auto count = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    return count > 0 ? static_cast<std::uint64_t>(count) : 0;
}

[[nodiscard]] std::uint64_t timestamp_microseconds(JobClock::time_point origin,
                                                   JobClock::time_point point) noexcept {
    return elapsed_microseconds(origin, point) + 1;
}

void update_maximum(std::atomic<std::uint64_t>& target, std::uint64_t candidate) noexcept {
    auto current = target.load(std::memory_order_relaxed);
    while (current < candidate &&
           !target.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

void request_cancellation(const std::shared_ptr<std::atomic<JobCancellationReason>>& cancellation,
                          JobCancellationReason reason) noexcept {
    auto expected = JobCancellationReason::none;
    static_cast<void>(cancellation->compare_exchange_strong(
        expected, reason, std::memory_order_release, std::memory_order_relaxed));
}

class ThreadPoolJobSystem final : public IJobSystem {
  public:
    explicit ThreadPoolJobSystem(JobSystemDesc desc)
        : desc_(desc), origin_(JobClock::now()), next_job_id_(desc.first_job_id) {
        workers_.reserve(desc_.worker_count);
        try {
            for (std::uint32_t index = 0; index < desc_.worker_count; ++index) {
                workers_.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            stopping_.store(true);
            jobs_ready_.notify_all();
            for (auto& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            throw;
        }
    }

    ThreadPoolJobSystem(const ThreadPoolJobSystem&) = delete;
    ThreadPoolJobSystem& operator=(const ThreadPoolJobSystem&) = delete;

    ~ThreadPoolJobSystem() override {
        {
            std::lock_guard lock(jobs_mutex_);
            stopping_.store(true);
            for (const auto& job : queued_jobs_) {
                request_cancellation(job.cancellation, JobCancellationReason::shutdown);
            }
            for (const auto& [id, cancellation] : running_cancellations_) {
                static_cast<void>(id);
                request_cancellation(cancellation, JobCancellationReason::shutdown);
            }
        }
        jobs_ready_.notify_all();
        completed_space_available_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    [[nodiscard]] JobBackend backend() const noexcept override {
        return JobBackend::thread_pool;
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return job_backend_name(JobBackend::thread_pool);
    }

    [[nodiscard]] std::uint32_t pending_count() const noexcept override {
        return static_cast<std::uint32_t>(pending_count_.load());
    }

    [[nodiscard]] std::uint64_t submitted_count() const noexcept override {
        return submitted_count_.load();
    }

    [[nodiscard]] std::uint64_t completed_count() const noexcept override {
        return completed_count_.load();
    }

    [[nodiscard]] JobSystemStats stats() const noexcept override {
        JobSystemStats result;
        {
            std::scoped_lock lock(jobs_mutex_, completed_mutex_);
            result.pending_jobs = static_cast<std::uint32_t>(pending_count_.load());
            result.queued_jobs = static_cast<std::uint32_t>(queued_jobs_.size());
            result.running_jobs = static_cast<std::uint32_t>(running_cancellations_.size());
            result.publishing_jobs = static_cast<std::uint32_t>(publishing_count_.load());
            result.completed_results = static_cast<std::uint32_t>(completed_results_.size());
            const auto now = JobClock::now();
            for (const auto& job : queued_jobs_) {
                result.oldest_queued_job_age_us = std::max(
                    result.oldest_queued_job_age_us, elapsed_microseconds(job.enqueued_at, now));
            }
        }
        result.max_pending_jobs = desc_.max_pending_jobs;
        result.max_completed_results = desc_.max_completed_results;
        result.submitted_jobs = submitted_count_.load();
        result.completed_jobs = completed_count_.load();
        result.rejected_submissions = rejected_submission_count_.load();
        result.cancellation_requests = cancellation_request_count_.load();
        result.cancelled_jobs = cancelled_count_.load();
        result.maximum_queue_latency_us = maximum_queue_latency_us_.load();
        return result;
    }

    [[nodiscard]] core::Result<JobId> submit(JobDesc desc) override {
        auto status = validate_job_desc(desc);
        if (!status) {
            return core::Result<JobId>::failure(status.error().code, status.error().message);
        }

        {
            std::lock_guard completed_lock(completed_mutex_);
            if (completed_results_.size() >= desc_.max_completed_results) {
                ++rejected_submission_count_;
                return core::Result<JobId>::failure("jobs.completed_queue_full",
                                                    "completed job result queue is full");
            }
        }

        QueuedJob queued;
        queued.name = std::move(desc.name);
        queued.type = desc.type.empty() ? queued.name : std::move(desc.type);
        queued.priority = desc.priority;
        queued.estimated_cost = desc.estimated_cost;
        queued.work = std::move(desc.work);
        queued.cancellation =
            std::make_shared<std::atomic<JobCancellationReason>>(JobCancellationReason::none);
        queued.enqueued_at = JobClock::now();

        core::Result<JobId> id =
            core::Result<JobId>::failure("jobs.internal_error", "job id was not assigned");
        [[maybe_unused]] std::uint32_t queued_count = 0;
        {
            std::lock_guard lock(jobs_mutex_);
            if (stopping_.load()) {
                ++rejected_submission_count_;
                return core::Result<JobId>::failure("jobs.stopping", "job system is shutting down");
            }
            if (pending_count_.load() >= desc_.max_pending_jobs) {
                ++rejected_submission_count_;
                HEARTSTEAD_PROFILE_PLOT("jobs.backpressure", rejected_submission_count_.load());
                return core::Result<JobId>::failure("jobs.pending_queue_full",
                                                    "pending job limit is full");
            }
            id = next_job_id();
            if (!id) {
                return id;
            }
            queued.id = id.value();
            queued.enqueue_sequence = submitted_count_.load() + 1;
            queued.enqueued_dispatch_count = dispatch_count_;
            queued_jobs_.push_back(std::move(queued));
            queued_count = static_cast<std::uint32_t>(queued_jobs_.size());
            ++submitted_count_;
            ++pending_count_;
        }

        HEARTSTEAD_PROFILE_PLOT("jobs.pending", pending_count_.load());
        HEARTSTEAD_PROFILE_PLOT("jobs.queued", queued_count);
        jobs_ready_.notify_one();
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

        {
            std::scoped_lock lock(jobs_mutex_, completed_mutex_);
            const auto queued = std::ranges::find(queued_jobs_, id, &QueuedJob::id);
            if (queued != queued_jobs_.end()) {
                request_cancellation(queued->cancellation, reason);
                if (completed_results_.size() < desc_.max_completed_results) {
                    const auto completed_at = JobClock::now();
                    JobResult result;
                    result.id = queued->id;
                    result.name = std::move(queued->name);
                    result.type = std::move(queued->type);
                    result.priority = queued->priority;
                    result.estimated_cost = queued->estimated_cost;
                    result.state = JobState::cancelled;
                    result.cancellation_reason = queued->cancellation->load();
                    result.enqueued_at_us = timestamp_microseconds(origin_, queued->enqueued_at);
                    result.completed_at_us = timestamp_microseconds(origin_, completed_at);
                    result.queue_latency_us =
                        elapsed_microseconds(queued->enqueued_at, completed_at);
                    result.total_latency_us = result.queue_latency_us;
                    update_maximum(maximum_queue_latency_us_, result.queue_latency_us);
                    queued_jobs_.erase(queued);

                    result.completion_order = completed_count_.load() + 1;
                    completed_results_.push_back(std::move(result));
                    --pending_count_;
                    ++completed_count_;
                    ++cancelled_count_;
                    HEARTSTEAD_PROFILE_PLOT("jobs.pending", pending_count_.load());
                    HEARTSTEAD_PROFILE_PLOT("jobs.cancelled", cancelled_count_.load());
                } else {
                    jobs_ready_.notify_one();
                }
                return core::Status::ok();
            }

            const auto running = running_cancellations_.find(id.value());
            if (running != running_cancellations_.end()) {
                request_cancellation(running->second, reason);
                return core::Status::ok();
            }
        }

        return core::Status::failure("jobs.not_active",
                                     "job is not queued or running in this job system");
    }

    [[nodiscard]] std::vector<JobResult> drain_completed() override {
        std::vector<JobResult> drained;
        {
            std::lock_guard lock(completed_mutex_);
            drained.reserve(completed_results_.size());
            while (!completed_results_.empty()) {
                drained.push_back(std::move(completed_results_.front()));
                completed_results_.pop_front();
            }
        }
        completed_space_available_.notify_all();
        return drained;
    }

  private:
    [[nodiscard]] core::Result<JobId> next_job_id() noexcept {
        auto candidate = next_job_id_.load(std::memory_order_relaxed);
        while (candidate != 0) {
            const auto successor = candidate == std::numeric_limits<std::uint64_t>::max()
                                       ? 0
                                       : candidate + 1;
            if (next_job_id_.compare_exchange_weak(candidate, successor,
                                                   std::memory_order_relaxed)) {
                return core::Result<JobId>::success(JobId::from_value(candidate));
            }
        }
        return core::Result<JobId>::failure("jobs.id_range_exhausted",
                                            "job id range is exhausted");
    }

    [[nodiscard]] QueuedJob take_next_job() {
        auto best = std::ranges::find_if(queued_jobs_, [](const QueuedJob& job) {
            return job.cancellation->load(std::memory_order_acquire) != JobCancellationReason::none;
        });
        if (best == queued_jobs_.end()) {
            const auto effective_priority = [this](const QueuedJob& job) {
                const auto waited_dispatches = dispatch_count_ - job.enqueued_dispatch_count;
                return std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(priority_rank(JobPriority::high)),
                    static_cast<std::uint64_t>(priority_rank(job.priority)) +
                        waited_dispatches / priority_aging_dispatches);
            };
            best = std::ranges::max_element(
                queued_jobs_, [&effective_priority](const QueuedJob& lhs, const QueuedJob& rhs) {
                    const auto lhs_priority = effective_priority(lhs);
                    const auto rhs_priority = effective_priority(rhs);
                    if (lhs_priority != rhs_priority) {
                        return lhs_priority < rhs_priority;
                    }
                    return lhs.enqueue_sequence > rhs.enqueue_sequence;
                });
        }
        QueuedJob job = std::move(*best);
        queued_jobs_.erase(best);
        if (dispatch_count_ != std::numeric_limits<std::uint64_t>::max()) {
            ++dispatch_count_;
        }
        return job;
    }

    void worker_loop() {
        HEARTSTEAD_PROFILE_THREAD_NAME("Heartstead job worker");
        while (true) {
            QueuedJob job;
            {
                std::unique_lock lock(jobs_mutex_);
                jobs_ready_.wait(lock,
                                 [this] { return stopping_.load() || !queued_jobs_.empty(); });
                if (stopping_.load() && queued_jobs_.empty()) {
                    return;
                }
                job = take_next_job();
                running_cancellations_.insert_or_assign(job.id.value(), job.cancellation);
            }

            const auto started_at = JobClock::now();
            JobResult result;
            result.id = job.id;
            result.name = job.name;
            result.type = job.type;
            result.priority = job.priority;
            result.estimated_cost = job.estimated_cost;
            result.state = JobState::running;
            result.enqueued_at_us = timestamp_microseconds(origin_, job.enqueued_at);
            result.started_at_us = timestamp_microseconds(origin_, started_at);
            result.queue_latency_us = elapsed_microseconds(job.enqueued_at, started_at);
            update_maximum(maximum_queue_latency_us_, result.queue_latency_us);

            HEARTSTEAD_PROFILE_ZONE_NAMED("jobs.execute");
            HEARTSTEAD_PROFILE_ZONE_TEXT(job.name.data(), job.name.size());
            HEARTSTEAD_PROFILE_ZONE_VALUE(job.id.value());

            if (job.cancellation->load(std::memory_order_acquire) == JobCancellationReason::none) {
                try {
                    const JobContext context{job.id, job.name, job.priority,
                                             job.cancellation.get()};
                    auto status = job.work(context);
                    if (status) {
                        result.state = JobState::succeeded;
                    } else {
                        result.state = JobState::failed;
                        result.error_code = status.error().code;
                        result.error_message = status.error().message;
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
            }

            const auto completed_at = JobClock::now();
            result.completed_at_us = timestamp_microseconds(origin_, completed_at);
            result.execution_duration_us = elapsed_microseconds(started_at, completed_at);
            result.total_latency_us = elapsed_microseconds(job.enqueued_at, completed_at);
            {
                std::lock_guard lock(jobs_mutex_);
                result.cancellation_reason = job.cancellation->load(std::memory_order_acquire);
                running_cancellations_.erase(job.id.value());
                ++publishing_count_;
            }
            if (result.cancellation_reason != JobCancellationReason::none) {
                result.state = JobState::cancelled;
                result.error_code.clear();
                result.error_message.clear();
                ++cancelled_count_;
                HEARTSTEAD_PROFILE_PLOT("jobs.cancelled", cancelled_count_.load());
            }
            publish_result(std::move(result));
        }
    }

    void publish_result(JobResult result) {
        {
            std::unique_lock lock(completed_mutex_);
            completed_space_available_.wait(lock, [this] {
                return stopping_.load() || completed_results_.size() < desc_.max_completed_results;
            });
            if (stopping_.load() && completed_results_.size() >= desc_.max_completed_results) {
                --publishing_count_;
                --pending_count_;
                HEARTSTEAD_PROFILE_PLOT("jobs.pending", pending_count_.load());
                return;
            }

            result.completion_order = completed_count_.load() + 1;
            completed_results_.push_back(std::move(result));
            --publishing_count_;
            --pending_count_;
            ++completed_count_;
            HEARTSTEAD_PROFILE_PLOT("jobs.pending", pending_count_.load());
            HEARTSTEAD_PROFILE_PLOT("jobs.maximum_queue_latency_us",
                                    maximum_queue_latency_us_.load());
        }
    }

    JobSystemDesc desc_;
    JobClock::time_point origin_;
    std::atomic<std::uint64_t> next_job_id_;
    std::atomic<std::uint64_t> submitted_count_ = 0;
    std::atomic<std::uint64_t> completed_count_ = 0;
    std::atomic<std::uint64_t> pending_count_ = 0;
    std::atomic<std::uint64_t> publishing_count_ = 0;
    std::atomic<std::uint64_t> rejected_submission_count_ = 0;
    std::atomic<std::uint64_t> cancellation_request_count_ = 0;
    std::atomic<std::uint64_t> cancelled_count_ = 0;
    std::atomic<std::uint64_t> maximum_queue_latency_us_ = 0;

    mutable std::mutex jobs_mutex_;
    std::condition_variable jobs_ready_;
    std::deque<QueuedJob> queued_jobs_;
    std::unordered_map<std::uint64_t, std::shared_ptr<std::atomic<JobCancellationReason>>>
        running_cancellations_;
    std::uint64_t dispatch_count_ = 0;
    std::atomic_bool stopping_ = false;

    mutable std::mutex completed_mutex_;
    std::condition_variable completed_space_available_;
    std::deque<JobResult> completed_results_;
    std::vector<std::thread> workers_;
};

} // namespace

JobBackendInfo backend_info() noexcept {
    return JobBackendInfo{
        JobBackend::thread_pool,
        job_backend_name(JobBackend::thread_pool),
        true,
        "available",
    };
}

core::Result<std::unique_ptr<IJobSystem>> create_job_system(JobSystemDesc desc) {
    try {
        return core::Result<std::unique_ptr<IJobSystem>>::success(
            std::make_unique<ThreadPoolJobSystem>(desc));
    } catch (const std::exception& exception) {
        return core::Result<std::unique_ptr<IJobSystem>>::failure(
            "jobs.thread_pool_start_failed",
            std::string("failed to start thread pool backend: ") + exception.what());
    }
}

} // namespace heartstead::jobs::thread_pool
