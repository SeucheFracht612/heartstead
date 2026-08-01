#include "engine/jobs/job_system.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace heartstead;
using namespace std::chrono_literals;

[[nodiscard]] bool wait_for_completed(jobs::IJobSystem& system, std::uint64_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (system.completed_count() < count && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return system.completed_count() >= count;
}

[[nodiscard]] const jobs::JobResult& find_result(const std::vector<jobs::JobResult>& results,
                                                 jobs::JobId id) {
    const auto found = std::ranges::find(results, id, &jobs::JobResult::id);
    assert(found != results.end());
    return *found;
}

void test_contract_validation_and_immediate_metadata() {
    jobs::JobSystemDesc invalid_system;
    invalid_system.max_pending_jobs = 0;
    const auto invalid_system_status = jobs::validate_job_system_desc(invalid_system);
    assert(!invalid_system_status);
    assert(invalid_system_status.error().code == "jobs.invalid_pending_limit");

    jobs::JobDesc invalid_job{"invalid-cost", jobs::JobPriority::normal,
                              [](const jobs::JobContext&) { return core::Status::ok(); }};
    invalid_job.estimated_cost = 0;
    const auto invalid_job_status = jobs::validate_job_desc(invalid_job);
    assert(!invalid_job_status);
    assert(invalid_job_status.error().code == "jobs.invalid_estimated_cost");

    auto system = jobs::create_job_system({jobs::JobBackend::immediate, 1, 2, 1, 1});
    assert(system);
    auto submitted = system.value()->submit(jobs::JobDesc{
        "world.generate.spawn", jobs::JobPriority::high,
        [](const jobs::JobContext& context) {
            assert(!context.cancellation_requested());
            assert(context.cancellation_reason() == jobs::JobCancellationReason::none);
            return core::Status::ok();
        },
        "world.generate", 17});
    assert(submitted);

    auto results = system.value()->drain_completed();
    assert(results.size() == 1);
    const auto& result = results.front();
    assert(result.type == "world.generate");
    assert(result.estimated_cost == 17);
    assert(result.enqueued_at_us != 0);
    assert(result.started_at_us >= result.enqueued_at_us);
    assert(result.completed_at_us >= result.started_at_us);
    assert(result.total_latency_us >= result.queue_latency_us);
    assert(result.cancellation_reason == jobs::JobCancellationReason::none);

    const auto stats = system.value()->stats();
    assert(stats.pending_jobs == 0);
    assert(stats.completed_results == 0);
    assert(stats.max_pending_jobs == 1);
    assert(stats.max_completed_results == 2);
    assert(stats.submitted_jobs == 1);
    assert(stats.completed_jobs == 1);

    const auto completed_cancel = system.value()->request_cancel(submitted.value());
    assert(!completed_cancel);
    assert(completed_cancel.error().code == "jobs.not_active");
    assert(jobs::job_cancellation_reason_name(jobs::JobCancellationReason::superseded) ==
           "superseded");
}

void test_pending_backpressure_and_queued_cancellation() {
    auto system = jobs::create_job_system({jobs::JobBackend::thread_pool, 1, 16, 1, 2});
    assert(system);

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool blocker_started = false;
    bool release_blocker = false;
    auto blocker = system.value()->submit(
        jobs::JobDesc{"blocking", jobs::JobPriority::normal,
                      [&](const jobs::JobContext&) {
                          std::unique_lock lock(gate_mutex);
                          blocker_started = true;
                          gate_changed.notify_all();
                          gate_changed.wait(lock, [&release_blocker] { return release_blocker; });
                          return core::Status::ok();
                      },
                      "test.blocking", 5});
    assert(blocker);
    {
        std::unique_lock lock(gate_mutex);
        assert(gate_changed.wait_for(lock, 2s, [&blocker_started] { return blocker_started; }));
    }

    std::atomic_bool queued_ran = false;
    auto queued = system.value()->submit(jobs::JobDesc{"speculative", jobs::JobPriority::low,
                                                       [&queued_ran](const jobs::JobContext&) {
                                                           queued_ran.store(true);
                                                           return core::Status::ok();
                                                       },
                                                       "test.speculative", 3});
    assert(queued);

    const auto saturated = system.value()->submit(
        jobs::JobDesc{"urgent", jobs::JobPriority::high,
                      [](const jobs::JobContext&) { return core::Status::ok(); }});
    assert(!saturated);
    assert(saturated.error().code == "jobs.pending_queue_full");
    auto saturated_stats = system.value()->stats();
    assert(saturated_stats.pending_jobs == 2);
    assert(saturated_stats.queued_jobs == 1);
    assert(saturated_stats.running_jobs == 1);
    assert(saturated_stats.publishing_jobs == 0);
    assert(saturated_stats.rejected_submissions == 1);

    assert(system.value()->request_cancel(queued.value(), jobs::JobCancellationReason::superseded));
    assert(!queued_ran.load());
    assert(system.value()->pending_count() == 1);

    auto urgent = system.value()->submit(jobs::JobDesc{
        "urgent", jobs::JobPriority::high,
        [](const jobs::JobContext&) { return core::Status::ok(); }, "test.urgent", 2});
    assert(urgent);
    {
        std::lock_guard lock(gate_mutex);
        release_blocker = true;
    }
    gate_changed.notify_all();

    assert(wait_for_completed(*system.value(), 3));
    const auto results = system.value()->drain_completed();
    assert(results.size() == 3);
    const auto& cancelled = find_result(results, queued.value());
    assert(cancelled.state == jobs::JobState::cancelled);
    assert(cancelled.cancellation_reason == jobs::JobCancellationReason::superseded);
    assert(cancelled.started_at_us == 0);
    assert(cancelled.execution_duration_us == 0);
    assert(find_result(results, blocker.value()).state == jobs::JobState::succeeded);
    assert(find_result(results, urgent.value()).state == jobs::JobState::succeeded);

    const auto final_stats = system.value()->stats();
    assert(final_stats.pending_jobs == 0);
    assert(final_stats.queued_jobs == 0);
    assert(final_stats.running_jobs == 0);
    assert(final_stats.publishing_jobs == 0);
    assert(final_stats.completed_results == 0);
    assert(final_stats.submitted_jobs == 3);
    assert(final_stats.completed_jobs == 3);
    assert(final_stats.cancelled_jobs == 1);
}

void test_running_cancellation_is_cooperative() {
    auto system = jobs::create_job_system({jobs::JobBackend::thread_pool, 1, 4, 1, 2});
    assert(system);

    std::mutex started_mutex;
    std::condition_variable started_changed;
    bool started = false;
    std::atomic observed_reason = jobs::JobCancellationReason::none;
    auto running = system.value()->submit(jobs::JobDesc{
        "cooperative", jobs::JobPriority::normal, [&](const jobs::JobContext& context) {
            {
                std::lock_guard lock(started_mutex);
                started = true;
            }
            started_changed.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!context.cancellation_requested() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            if (!context.cancellation_requested()) {
                return core::Status::failure("test.cancel_timeout",
                                             "cancellation was not observed");
            }
            observed_reason.store(context.cancellation_reason());
            return core::Status::ok();
        }});
    assert(running);
    {
        std::unique_lock lock(started_mutex);
        assert(started_changed.wait_for(lock, 2s, [&started] { return started; }));
    }

    assert(system.value()->request_cancel(running.value()));
    assert(wait_for_completed(*system.value(), 1));
    const auto results = system.value()->drain_completed();
    assert(results.size() == 1);
    assert(results.front().state == jobs::JobState::cancelled);
    assert(results.front().cancellation_reason == jobs::JobCancellationReason::requested);
    assert(results.front().started_at_us != 0);
    assert(observed_reason.load() == jobs::JobCancellationReason::requested);
    assert(system.value()->stats().cancelled_jobs == 1);
}

void test_cancellation_does_not_deadlock_a_full_result_mailbox() {
    auto system = jobs::create_job_system({jobs::JobBackend::thread_pool, 1, 1, 1, 2});
    assert(system);

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool blocker_started = false;
    bool release_blocker = false;
    auto blocker = system.value()->submit(
        jobs::JobDesc{"mailbox.blocker", jobs::JobPriority::normal, [&](const jobs::JobContext&) {
                          std::unique_lock lock(gate_mutex);
                          blocker_started = true;
                          gate_changed.notify_all();
                          gate_changed.wait(lock, [&release_blocker] { return release_blocker; });
                          return core::Status::ok();
                      }});
    assert(blocker);
    {
        std::unique_lock lock(gate_mutex);
        assert(gate_changed.wait_for(lock, 2s, [&blocker_started] { return blocker_started; }));
    }

    std::atomic_bool queued_ran = false;
    auto queued = system.value()->submit(jobs::JobDesc{
        "mailbox.cancelled", jobs::JobPriority::normal, [&queued_ran](const jobs::JobContext&) {
            queued_ran.store(true);
            return core::Status::ok();
        }});
    assert(queued);
    assert(system.value()->request_cancel(queued.value()));
    assert(system.value()->completed_count() == 1);

    {
        std::lock_guard lock(gate_mutex);
        release_blocker = true;
    }
    gate_changed.notify_all();
    while (system.value()->pending_count() != 1) {
        std::this_thread::yield();
    }

    auto cancelled = system.value()->drain_completed();
    assert(cancelled.size() == 1);
    assert(cancelled.front().id == queued.value());
    assert(cancelled.front().state == jobs::JobState::cancelled);
    assert(!queued_ran.load());
    assert(wait_for_completed(*system.value(), 2));
    auto completed = system.value()->drain_completed();
    assert(completed.size() == 1);
    assert(completed.front().id == blocker.value());
    assert(completed.front().state == jobs::JobState::succeeded);
    assert(system.value()->pending_count() == 0);
}

void test_priority_aging_bounds_starvation() {
    auto system = jobs::create_job_system({jobs::JobBackend::thread_pool, 1, 32, 1, 32});
    assert(system);

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool blocker_started = false;
    bool release_blocker = false;
    auto blocker = system.value()->submit(
        jobs::JobDesc{"aging.blocker", jobs::JobPriority::normal, [&](const jobs::JobContext&) {
                          std::unique_lock lock(gate_mutex);
                          blocker_started = true;
                          gate_changed.notify_all();
                          gate_changed.wait(lock, [&release_blocker] { return release_blocker; });
                          return core::Status::ok();
                      }});
    assert(blocker);
    {
        std::unique_lock lock(gate_mutex);
        assert(gate_changed.wait_for(lock, 2s, [&blocker_started] { return blocker_started; }));
    }

    auto low = system.value()->submit(
        jobs::JobDesc{"aging.low", jobs::JobPriority::low,
                      [](const jobs::JobContext&) { return core::Status::ok(); }});
    assert(low);
    for (std::uint32_t index = 0; index < 20; ++index) {
        auto high = system.value()->submit(
            jobs::JobDesc{"aging.high", jobs::JobPriority::high,
                          [](const jobs::JobContext&) { return core::Status::ok(); }});
        assert(high);
    }

    {
        std::lock_guard lock(gate_mutex);
        release_blocker = true;
    }
    gate_changed.notify_all();
    assert(wait_for_completed(*system.value(), 22));
    const auto results = system.value()->drain_completed();
    assert(results.size() == 22);
    const auto& low_result = find_result(results, low.value());
    assert(low_result.state == jobs::JobState::succeeded);
    assert(low_result.completion_order <= 18);
    assert(system.value()->pending_count() == 0);
}

void test_shutdown_cancels_running_and_queued_work() {
    auto system = jobs::create_job_system({jobs::JobBackend::thread_pool, 1, 1, 1, 4});
    assert(system);

    std::mutex started_mutex;
    std::condition_variable started_changed;
    bool started = false;
    std::atomic observed_reason = jobs::JobCancellationReason::none;
    auto running = system.value()->submit(jobs::JobDesc{
        "shutdown.running", jobs::JobPriority::normal, [&](const jobs::JobContext& context) {
            {
                std::lock_guard lock(started_mutex);
                started = true;
            }
            started_changed.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!context.cancellation_requested() &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            observed_reason.store(context.cancellation_reason());
            return core::Status::ok();
        }});
    assert(running);
    {
        std::unique_lock lock(started_mutex);
        assert(started_changed.wait_for(lock, 2s, [&started] { return started; }));
    }

    std::atomic_uint32_t queued_callbacks = 0;
    for (std::uint32_t index = 0; index < 2; ++index) {
        auto queued =
            system.value()->submit(jobs::JobDesc{"shutdown.queued", jobs::JobPriority::normal,
                                                 [&queued_callbacks](const jobs::JobContext&) {
                                                     ++queued_callbacks;
                                                     return core::Status::ok();
                                                 }});
        assert(queued);
    }

    system.value().reset();
    assert(observed_reason.load() == jobs::JobCancellationReason::shutdown);
    assert(queued_callbacks.load() == 0);
}

} // namespace

int main() {
    test_contract_validation_and_immediate_metadata();
    test_pending_backpressure_and_queued_cancellation();
    test_running_cancellation_is_cooperative();
    test_cancellation_does_not_deadlock_a_full_result_mailbox();
    test_priority_aging_bounds_starvation();
    test_shutdown_cancels_running_and_queued_work();
    return 0;
}
