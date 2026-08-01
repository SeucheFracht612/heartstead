#include "engine/jobs/thread_pool/thread_pool_backend.hpp"

#include "engine/profiling/profiler.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace heartstead::jobs::thread_pool {

namespace {

struct QueuedJob {
    JobId id;
    std::string name;
    JobPriority priority = JobPriority::normal;
    JobFunction work;
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

class ThreadPoolJobSystem final : public IJobSystem {
  public:
    explicit ThreadPoolJobSystem(JobSystemDesc desc)
        : desc_(desc), next_job_id_(desc.first_job_id) {
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
        stopping_.store(true);
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

    [[nodiscard]] core::Result<JobId> submit(JobDesc desc) override {
        auto status = validate_job_desc(desc);
        if (!status) {
            return core::Result<JobId>::failure(status.error().code, status.error().message);
        }

        {
            std::lock_guard completed_lock(completed_mutex_);
            if (completed_results_.size() >= desc_.max_completed_results) {
                return core::Result<JobId>::failure("jobs.completed_queue_full",
                                                    "completed job result queue is full");
            }
        }

        auto id = next_job_id();
        if (!id) {
            return id;
        }
        QueuedJob queued;
        queued.id = id.value();
        queued.name = std::move(desc.name);
        queued.priority = desc.priority;
        queued.work = std::move(desc.work);

        {
            std::lock_guard lock(jobs_mutex_);
            if (stopping_.load()) {
                return core::Result<JobId>::failure("jobs.stopping", "job system is shutting down");
            }
            queued_jobs_.push_back(std::move(queued));
            ++submitted_count_;
            ++pending_count_;
        }

        HEARTSTEAD_PROFILE_PLOT("jobs.pending", pending_count_.load());
        jobs_ready_.notify_one();
        return id;
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
        const auto best =
            std::ranges::max_element(queued_jobs_, [](const QueuedJob& lhs, const QueuedJob& rhs) {
                return priority_rank(lhs.priority) < priority_rank(rhs.priority);
            });
        QueuedJob job = std::move(*best);
        queued_jobs_.erase(best);
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
            }

            JobResult result;
            result.id = job.id;
            result.name = job.name;
            result.priority = job.priority;
            result.state = JobState::running;

            HEARTSTEAD_PROFILE_ZONE_NAMED("jobs.execute");
            HEARTSTEAD_PROFILE_ZONE_TEXT(job.name.data(), job.name.size());
            HEARTSTEAD_PROFILE_ZONE_VALUE(job.id.value());

            try {
                const JobContext context{job.id, job.name, job.priority, false};
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
                --pending_count_;
                HEARTSTEAD_PROFILE_PLOT("jobs.pending", pending_count_.load());
                return;
            }

            result.completion_order = completed_count_.load() + 1;
            completed_results_.push_back(std::move(result));
            --pending_count_;
            ++completed_count_;
            HEARTSTEAD_PROFILE_PLOT("jobs.pending", pending_count_.load());
        }
    }

    JobSystemDesc desc_;
    std::atomic<std::uint64_t> next_job_id_;
    std::atomic<std::uint64_t> submitted_count_ = 0;
    std::atomic<std::uint64_t> completed_count_ = 0;
    std::atomic<std::uint64_t> pending_count_ = 0;

    mutable std::mutex jobs_mutex_;
    std::condition_variable jobs_ready_;
    std::deque<QueuedJob> queued_jobs_;
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
