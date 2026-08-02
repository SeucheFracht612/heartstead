#include "engine/world/streaming/chunk_load_scheduler.hpp"

#include "engine/profiling/cpu_timing.hpp"
#include "engine/world/chunks/chunk_edit_delta_codec.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

namespace heartstead::world {

namespace {

using SchedulerClock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed_microseconds(SchedulerClock::time_point begin,
                                                 SchedulerClock::time_point end) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

[[nodiscard]] double elapsed_milliseconds(SchedulerClock::time_point begin,
                                          SchedulerClock::time_point end) noexcept {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

} // namespace

struct ChunkLoadScheduler::SharedContext {
    explicit SharedContext(ChunkLoadSchedulerContext source)
        : generation(std::move(source.generation)), regions(std::move(source.regions)),
          palette(std::move(source.palette)), saved_deltas(std::move(source.saved_deltas)) {}

    TerrainGenerationConfig generation;
    RegionGraph regions;
    VoxelPalette palette;
    std::shared_ptr<const IChunkEditDeltaSource> saved_deltas;
};

struct ChunkLoadScheduler::SharedState {
    explicit SharedState(std::size_t maximum_results) : max_results(maximum_results) {}

    void publish(ChunkLoadResult result) {
        std::lock_guard lock(mutex);
        if (completed.size() < max_results) {
            completed.push_back(std::move(result));
        }
    }

    [[nodiscard]] std::deque<ChunkLoadResult> drain() {
        std::lock_guard lock(mutex);
        std::deque<ChunkLoadResult> result;
        result.swap(completed);
        return result;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard lock(mutex);
        return completed.size();
    }

    std::size_t max_results = 0;
    mutable std::mutex mutex;
    std::deque<ChunkLoadResult> completed;
};

core::Status ChunkLoadSchedulerConfig::validate() const {
    if (worker_count == 0 || max_concurrent_requests == 0 || max_completed_results == 0 ||
        reservation_bytes_per_request == 0 || max_reserved_working_bytes == 0 ||
        max_publications_per_update == 0 || max_publication_time_us == 0 || first_request_id == 0) {
        return core::Status::failure(
            "chunk_load_scheduler.invalid_config",
            "chunk load worker, queue, memory, publication, and identifier limits must be nonzero");
    }
    if (max_completed_results < max_concurrent_requests) {
        return core::Status::failure(
            "chunk_load_scheduler.invalid_completed_limit",
            "completed-result capacity must cover every concurrent chunk load");
    }
    if (reservation_bytes_per_request > max_reserved_working_bytes) {
        return core::Status::failure(
            "chunk_load_scheduler.invalid_memory_budget",
            "per-load memory reservation cannot exceed the aggregate memory budget");
    }
    if (max_concurrent_requests > std::numeric_limits<std::uint32_t>::max() ||
        max_completed_results > std::numeric_limits<std::uint32_t>::max()) {
        return core::Status::failure("chunk_load_scheduler.limit_out_of_range",
                                     "chunk load queue limits exceed the job backend range");
    }
    return core::Status::ok();
}

core::Status ChunkLoadSchedulerContext::validate() const {
    if (generation.region_id.empty()) {
        return core::Status::failure("chunk_load_scheduler.missing_region_id",
                                     "chunk load generation requires a region id");
    }
    if (regions.find(generation.region_id) == nullptr) {
        return core::Status::failure("chunk_load_scheduler.missing_region",
                                     "chunk load generation region does not exist");
    }
    if (palette.empty()) {
        return core::Status::failure("chunk_load_scheduler.empty_palette",
                                     "chunk load generation requires a voxel palette");
    }
    return core::Status::ok();
}

std::size_t ChunkLoadPublicationReport::processed_count() const noexcept {
    return published.size() + cancelled.size() + stale.size() + failures.size();
}

core::Result<std::unique_ptr<ChunkLoadScheduler>>
ChunkLoadScheduler::create(ChunkLoadSchedulerContext context, ChunkLoadSchedulerConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<ChunkLoadScheduler>>::failure(status.error().code,
                                                                          status.error().message);
    }
    status = context.validate();
    if (!status) {
        return core::Result<std::unique_ptr<ChunkLoadScheduler>>::failure(status.error().code,
                                                                          status.error().message);
    }

    jobs::JobSystemDesc job_config;
    job_config.backend = jobs::JobBackend::thread_pool;
    job_config.worker_count = config.worker_count;
    job_config.max_pending_jobs = static_cast<std::uint32_t>(config.max_concurrent_requests);
    job_config.max_completed_results = static_cast<std::uint32_t>(config.max_completed_results);
    auto jobs = jobs::create_job_system(job_config);
    if (!jobs) {
        return core::Result<std::unique_ptr<ChunkLoadScheduler>>::failure(jobs.error().code,
                                                                          jobs.error().message);
    }
    auto shared_context = std::make_shared<const SharedContext>(std::move(context));
    auto shared_state = std::make_shared<SharedState>(config.max_completed_results);
    return core::Result<std::unique_ptr<ChunkLoadScheduler>>::success(
        std::unique_ptr<ChunkLoadScheduler>(new ChunkLoadScheduler(
            config, std::move(jobs).value(), std::move(shared_context), std::move(shared_state))));
}

ChunkLoadScheduler::ChunkLoadScheduler(ChunkLoadSchedulerConfig config,
                                       std::unique_ptr<jobs::IJobSystem> jobs,
                                       std::shared_ptr<const SharedContext> context,
                                       std::shared_ptr<SharedState> shared_state)
    : config_(config), jobs_(std::move(jobs)), context_(std::move(context)),
      shared_state_(std::move(shared_state)), next_request_id_(config.first_request_id) {}

ChunkLoadScheduler::~ChunkLoadScheduler() {
    shutdown();
}

core::Result<ChunkLoadRequestId> ChunkLoadScheduler::submit(ChunkCoord coord,
                                                            jobs::JobPriority priority) {
    if (jobs_ == nullptr) {
        ++stats_.rejected_requests;
        return core::Result<ChunkLoadRequestId>::failure("chunk_load_scheduler.stopped",
                                                         "chunk load scheduler is stopped");
    }
    if (active_by_coord_.contains(coord)) {
        ++stats_.duplicate_requests;
        return core::Result<ChunkLoadRequestId>::failure(
            "chunk_load_scheduler.duplicate_request",
            "a chunk load request is already active for this coordinate");
    }
    if (active_requests_.size() >= config_.max_concurrent_requests ||
        config_.reservation_bytes_per_request >
            config_.max_reserved_working_bytes - stats_.reserved_working_bytes) {
        ++stats_.rejected_requests;
        return core::Result<ChunkLoadRequestId>::failure(
            "chunk_load_scheduler.full",
            "chunk load scheduler reached its request or memory reservation budget");
    }
    if (next_request_id_ == 0) {
        ++stats_.rejected_requests;
        return core::Result<ChunkLoadRequestId>::failure(
            "chunk_load_scheduler.request_id_exhausted",
            "chunk load request identifier range is exhausted");
    }

    const auto request_id = ChunkLoadRequestId::from_value(next_request_id_);
    auto context = context_;
    auto shared = shared_state_;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    jobs::JobDesc job;
    job.name = "chunk_load";
    job.type = "streaming.chunk_load";
    job.priority = priority;
    job.estimated_cost = static_cast<std::uint32_t>(
        std::min(config_.reservation_bytes_per_request / 1024U + 1U,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    job.work = [request_id, coord, context, shared, cancellation,
                reservation = config_.reservation_bytes_per_request](
                   const jobs::JobContext& job_context) mutable {
        ChunkLoadResult result;
        result.request_id = request_id;
        result.coord = coord;
        result.reserved_working_bytes = reservation;
        const auto cancelled = [&] {
            return job_context.cancellation_requested() ||
                   cancellation->load(std::memory_order_acquire);
        };

        {
            profiling::ScopedCpuTimer worker_timer(result.worker_ms);
            try {
                const auto execute = [&] {
                    if (cancelled()) {
                        result.state = ChunkLoadResultState::cancelled;
                        return;
                    }

                    std::optional<save::ChunkEditSaveRecord> saved_delta;
                    if (context->saved_deltas != nullptr) {
                        auto read = [&] {
                            profiling::ScopedCpuTimer timer(result.disk_read_ms);
                            return context->saved_deltas->read_chunk_delta(coord);
                        }();
                        if (!read) {
                            result.state = ChunkLoadResultState::failed;
                            result.error_code = read.error().code;
                            result.error_message = read.error().message;
                            return;
                        }
                        saved_delta = std::move(read).value();
                    }
                    if (cancelled()) {
                        result.state = ChunkLoadResultState::cancelled;
                        return;
                    }

                    std::vector<VoxelEditRecord> edits;
                    if (saved_delta.has_value()) {
                        auto decoded = [&] {
                            profiling::ScopedCpuTimer timer(result.decode_ms);
                            return ChunkEditDeltaTextCodec::decode(coord,
                                                                   saved_delta->encoded_edit_delta);
                        }();
                        if (!decoded) {
                            result.state = ChunkLoadResultState::failed;
                            result.error_code = decoded.error().code;
                            result.error_message = decoded.error().message;
                            return;
                        }
                        edits = std::move(decoded).value();
                    }
                    if (cancelled()) {
                        result.state = ChunkLoadResultState::cancelled;
                        return;
                    }

                    auto generated = [&] {
                        profiling::ScopedCpuTimer timer(result.generation_ms);
                        return DeterministicTerrainGenerator::generate_chunk(
                            coord, context->generation, context->regions, context->palette);
                    }();
                    if (!generated) {
                        result.state = ChunkLoadResultState::failed;
                        result.error_code = generated.error().code;
                        result.error_message = generated.error().message;
                        return;
                    }
                    if (cancelled()) {
                        result.state = ChunkLoadResultState::cancelled;
                        return;
                    }

                    auto prepared = [&] {
                        profiling::ScopedCpuTimer timer(result.prepare_ms);
                        return ChunkDatabase::prepare_generated(std::move(generated).value(),
                                                                edits);
                    }();
                    if (!prepared) {
                        result.state = ChunkLoadResultState::failed;
                        result.error_code = prepared.error().code;
                        result.error_message = prepared.error().message;
                        return;
                    }
                    if (cancelled()) {
                        result.state = ChunkLoadResultState::cancelled;
                        return;
                    }
                    result.source = edits.empty()
                                        ? ChunkStreamLoadSource::generated
                                        : ChunkStreamLoadSource::generated_with_saved_delta;
                    result.saved_edit_count = edits.size();
                    result.prepared.emplace(std::move(prepared).value());
                    result.state = ChunkLoadResultState::succeeded;
                };
                execute();
            } catch (const std::exception& exception) {
                result.state = ChunkLoadResultState::failed;
                result.error_code = "chunk_load_scheduler.worker_exception";
                result.error_message = exception.what();
            } catch (...) {
                result.state = ChunkLoadResultState::failed;
                result.error_code = "chunk_load_scheduler.worker_exception";
                result.error_message = "chunk load worker threw an unknown exception";
            }
        }
        shared->publish(std::move(result));
        return core::Status::ok();
    };

    auto submitted = jobs_->submit(std::move(job));
    if (!submitted) {
        ++stats_.rejected_requests;
        return core::Result<ChunkLoadRequestId>::failure(submitted.error().code,
                                                         submitted.error().message);
    }
    auto active = ActiveRequest{submitted.value(), coord, config_.reservation_bytes_per_request,
                                SchedulerClock::now(), std::move(cancellation)};
    active_by_coord_.emplace(coord, request_id);
    active_requests_.emplace(request_id, std::move(active));
    stats_.reserved_working_bytes += config_.reservation_bytes_per_request;
    stats_.reserved_working_bytes_high_water =
        std::max(stats_.reserved_working_bytes_high_water, stats_.reserved_working_bytes);
    ++stats_.submitted_requests;
    next_request_id_ =
        next_request_id_ == std::numeric_limits<std::uint64_t>::max() ? 0 : next_request_id_ + 1;
    refresh_stats();
    return core::Result<ChunkLoadRequestId>::success(request_id);
}

core::Status ChunkLoadScheduler::cancel(ChunkCoord coord) noexcept {
    const auto request = active_by_coord_.find(coord);
    if (request == active_by_coord_.end()) {
        return core::Status::failure("chunk_load_scheduler.request_missing",
                                     "chunk coordinate has no active load request");
    }
    const auto active = active_requests_.find(request->second);
    if (active == active_requests_.end()) {
        return core::Status::failure("chunk_load_scheduler.request_missing",
                                     "chunk load request bookkeeping is stale");
    }
    active->second.cancellation->store(true, std::memory_order_release);
    return core::Status::ok();
}

std::size_t ChunkLoadScheduler::cancel_all_except(std::span<const ChunkCoord> desired) {
    const std::set<ChunkCoord> retained(desired.begin(), desired.end());
    std::size_t cancelled = 0;
    for (const auto& [coord, request_id] : active_by_coord_) {
        if (retained.contains(coord)) {
            continue;
        }
        const auto active = active_requests_.find(request_id);
        if (active != active_requests_.end() &&
            !active->second.cancellation->exchange(true, std::memory_order_acq_rel)) {
            ++cancelled;
        }
    }
    return cancelled;
}

void ChunkLoadScheduler::cancel_all() noexcept {
    for (const auto& [_, request] : active_requests_) {
        request.cancellation->store(true, std::memory_order_release);
    }
}

void ChunkLoadScheduler::collect_completed(ChunkLoadPublicationReport& report) {
    if (jobs_ != nullptr) {
        static_cast<void>(jobs_->drain_completed());
    }
    auto completed = shared_state_->drain();
    report.collected_worker_results = completed.size();
    while (!completed.empty()) {
        ready_for_publication_.push_back(std::move(completed.front()));
        completed.pop_front();
    }
}

ChunkLoadPublicationReport ChunkLoadScheduler::update(WorldState& state) {
    ChunkLoadPublicationReport report;
    collect_completed(report);
    const auto started_at = SchedulerClock::now();

    while (!ready_for_publication_.empty() &&
           report.processed_count() < config_.max_publications_per_update) {
        const auto elapsed = elapsed_microseconds(started_at, SchedulerClock::now());
        if (report.processed_count() != 0 && elapsed >= config_.max_publication_time_us) {
            report.time_budget_exhausted = true;
            break;
        }

        auto result = std::move(ready_for_publication_.front());
        ready_for_publication_.pop_front();
        const auto active = active_requests_.find(result.request_id);
        if (active != active_requests_.end()) {
            result.pipeline_latency_ms =
                elapsed_milliseconds(active->second.submitted_at, SchedulerClock::now());
            if (active->second.cancellation->load(std::memory_order_acquire)) {
                result.state = ChunkLoadResultState::cancelled;
                result.prepared.reset();
            }
        }

        if (result.state == ChunkLoadResultState::cancelled) {
            report.cancelled.push_back(result.coord);
        } else if (result.state == ChunkLoadResultState::failed || !result.prepared.has_value()) {
            result.state = ChunkLoadResultState::failed;
            const auto code = result.error_code.empty()
                                  ? std::string("chunk_load_scheduler.missing_product")
                                  : result.error_code;
            const auto message = result.error_message.empty()
                                     ? std::string("successful chunk load has no prepared product")
                                     : result.error_message;
            report.failures.push_back(
                {result.request_id, result.coord, {std::move(code), std::move(message)}});
        } else if (state.chunks().contains(result.coord)) {
            report.stale.push_back(result.coord);
            ++stats_.stale_requests;
        } else {
            auto status = state.chunks().insert_prepared_generated(std::move(*result.prepared),
                                                                   state.dirty_regions());
            if (!status) {
                result.state = ChunkLoadResultState::failed;
                report.failures.push_back({result.request_id, result.coord, status.error()});
            } else {
                const auto* chunk = state.chunks().find(result.coord);
                ChunkStreamLoadReport published;
                published.coord = result.coord;
                published.identity = chunk == nullptr ? ChunkIdentity{} : chunk->identity();
                published.source = result.source;
                published.generated_chunk_inserted = true;
                published.saved_delta_applied = result.saved_edit_count != 0;
                published.saved_edit_count = result.saved_edit_count;
                report.published.push_back(published);
                ++stats_.published_requests;
            }
        }

        stats_.last_disk_read_ms = result.disk_read_ms;
        stats_.last_decode_ms = result.decode_ms;
        stats_.last_generation_ms = result.generation_ms;
        stats_.last_prepare_ms = result.prepare_ms;
        stats_.last_worker_ms = result.worker_ms;
        stats_.last_pipeline_latency_ms = result.pipeline_latency_ms;
        stats_.maximum_pipeline_latency_ms =
            std::max(stats_.maximum_pipeline_latency_ms, result.pipeline_latency_ms);
        finish_request(result);
    }

    report.publication_time_us = elapsed_microseconds(started_at, SchedulerClock::now());
    stats_.maximum_publication_time_us =
        std::max(stats_.maximum_publication_time_us, report.publication_time_us);
    report.item_budget_exhausted = !ready_for_publication_.empty() &&
                                   report.processed_count() >= config_.max_publications_per_update;
    if (!ready_for_publication_.empty() &&
        report.publication_time_us >= config_.max_publication_time_us) {
        report.time_budget_exhausted = true;
    }
    refresh_stats();
    return report;
}

void ChunkLoadScheduler::finish_request(const ChunkLoadResult& result) noexcept {
    const auto active = active_requests_.find(result.request_id);
    if (active != active_requests_.end()) {
        stats_.reserved_working_bytes -= active->second.reserved_working_bytes;
        active_by_coord_.erase(active->second.coord);
        active_requests_.erase(active);
    }
    if (result.state == ChunkLoadResultState::cancelled) {
        ++stats_.cancelled_requests;
    } else if (result.state == ChunkLoadResultState::failed) {
        ++stats_.failed_requests;
    }
}

bool ChunkLoadScheduler::has_capacity() const noexcept {
    return jobs_ != nullptr && active_requests_.size() < config_.max_concurrent_requests &&
           config_.reservation_bytes_per_request <=
               config_.max_reserved_working_bytes - stats_.reserved_working_bytes;
}

bool ChunkLoadScheduler::has_in_flight() const noexcept {
    return !active_requests_.empty();
}

bool ChunkLoadScheduler::contains(ChunkCoord coord) const noexcept {
    return active_by_coord_.contains(coord);
}

const ChunkLoadSchedulerStats& ChunkLoadScheduler::stats() noexcept {
    refresh_stats();
    return stats_;
}

void ChunkLoadScheduler::refresh_stats() noexcept {
    stats_.in_flight_requests = active_requests_.size();
    stats_.completed_mailbox_count = shared_state_->size();
    stats_.ready_for_publication_count = ready_for_publication_.size();
    stats_.oldest_queued_request_age_us =
        jobs_ == nullptr ? 0 : jobs_->stats().oldest_queued_job_age_us;
}

void ChunkLoadScheduler::shutdown() noexcept {
    if (jobs_ == nullptr) {
        return;
    }
    cancel_all();
    jobs_.reset();
    static_cast<void>(shared_state_->drain());
    ready_for_publication_.clear();
    active_by_coord_.clear();
    active_requests_.clear();
    stats_.reserved_working_bytes = 0;
    refresh_stats();
}

const char* chunk_load_result_state_name(ChunkLoadResultState state) noexcept {
    switch (state) {
    case ChunkLoadResultState::succeeded:
        return "succeeded";
    case ChunkLoadResultState::failed:
        return "failed";
    case ChunkLoadResultState::cancelled:
        return "cancelled";
    }
    return "unknown";
}

} // namespace heartstead::world
