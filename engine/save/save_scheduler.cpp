#include "engine/save/save_scheduler.hpp"

#include "engine/profiling/cpu_timing.hpp"
#include "engine/save/save_slot.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <string_view>
#include <utility>

namespace heartstead::save {

namespace {

class ByteCounter {
  public:
    void add(std::size_t bytes) noexcept {
        if (bytes > std::numeric_limits<std::size_t>::max() - bytes_) {
            bytes_ = std::numeric_limits<std::size_t>::max();
            saturated_ = true;
            return;
        }
        bytes_ += bytes;
    }

    void add_product(std::size_t count, std::size_t element_bytes) noexcept {
        if (count != 0 && element_bytes > std::numeric_limits<std::size_t>::max() / count) {
            bytes_ = std::numeric_limits<std::size_t>::max();
            saturated_ = true;
            return;
        }
        add(count * element_bytes);
    }

    [[nodiscard]] std::size_t value() const noexcept {
        return bytes_;
    }

    [[nodiscard]] bool saturated() const noexcept {
        return saturated_;
    }

  private:
    std::size_t bytes_ = 0;
    bool saturated_ = false;
};

void count_string(ByteCounter& counter, const std::string& value) noexcept {
    // Counting capacity even for an implementation's inline string storage is deliberately
    // conservative and covers allocator bookkeeping that is otherwise implementation-specific.
    counter.add(value.capacity() + 1U);
}

void count_prototype(ByteCounter& counter, const core::PrototypeId& value) noexcept {
    count_string(counter, value.value());
}

template <typename Value>
void count_vector_storage(ByteCounter& counter, const std::vector<Value>& values) noexcept {
    counter.add_product(values.capacity(), sizeof(Value));
}

void count_string_vector(ByteCounter& counter, const std::vector<std::string>& values) noexcept {
    count_vector_storage(counter, values);
    for (const auto& value : values) {
        count_string(counter, value);
    }
}

void count_metadata(ByteCounter& counter, const SaveMetadata& metadata) noexcept {
    count_string(counter, metadata.game_version);
    count_vector_storage(counter, metadata.enabled_mods);
    for (const auto& mod : metadata.enabled_mods) {
        count_string(counter, mod.id);
        count_string(counter, mod.version);
        count_string(counter, mod.prototype_hash);
    }
    count_string_vector(counter, metadata.migration_history);
}

void count_snapshot_payload(ByteCounter& counter, const SaveSnapshot& snapshot) noexcept {
    count_metadata(counter, snapshot.metadata);

    count_vector_storage(counter, snapshot.voxel_palette.entries);
    for (const auto& entry : snapshot.voxel_palette.entries) {
        count_prototype(counter, entry.prototype_id);
    }

    count_vector_storage(counter, snapshot.chunk_edits);
    for (const auto& chunk : snapshot.chunk_edits) {
        count_string(counter, chunk.encoded_edit_delta);
    }

    count_vector_storage(counter, snapshot.build_pieces);
    for (const auto& piece : snapshot.build_pieces) {
        count_prototype(counter, piece.prototype_id);
        count_vector_storage(counter, piece.sockets);
        for (const auto& socket : piece.sockets) {
            count_string(counter, socket.name);
            count_string(counter, socket.tag);
        }
        count_vector_storage(counter, piece.network_ports);
        for (const auto& port : piece.network_ports) {
            count_string(counter, port.name);
        }
        count_string_vector(counter, piece.material_tags);
        count_string_vector(counter, piece.room_contribution_tags);
    }

    count_vector_storage(counter, snapshot.entities);
    for (const auto& entity : snapshot.entities) {
        count_prototype(counter, entity.prototype_id);
        count_string(counter, entity.encoded_state);
    }

    count_vector_storage(counter, snapshot.inventories);
    for (const auto& inventory : snapshot.inventories) {
        count_vector_storage(counter, inventory.stacks);
        for (const auto& stack : inventory.stacks) {
            count_prototype(counter, stack.prototype_id);
        }
    }

    count_vector_storage(counter, snapshot.cargo_records);
    for (const auto& cargo : snapshot.cargo_records) {
        count_prototype(counter, cargo.prototype_id);
        count_string_vector(counter, cargo.hazard_tags);
    }

    count_vector_storage(counter, snapshot.workpieces);
    for (const auto& workpiece : snapshot.workpieces) {
        count_prototype(counter, workpiece.prototype_id);
        count_string(counter, workpiece.encoded_cells);
        count_prototype(counter, workpiece.material_prototype_id);
        count_string(counter, workpiece.encoded_server_state);
    }

    count_vector_storage(counter, snapshot.assemblies);
    for (const auto& assembly : snapshot.assemblies) {
        count_prototype(counter, assembly.prototype_id);
        count_vector_storage(counter, assembly.parts);
        for (const auto& part : assembly.parts) {
            count_string(counter, part.name);
            count_prototype(counter, part.prototype_id);
        }
        count_vector_storage(counter, assembly.ports);
        for (const auto& port : assembly.ports) {
            count_string(counter, port.name);
        }
        count_string_vector(counter, assembly.capabilities);
        count_vector_storage(counter, assembly.process_slots);
        count_string(counter, assembly.failure_reason);
        count_string(counter, assembly.custom_state);
    }

    count_vector_storage(counter, snapshot.processes);
    for (const auto& process : snapshot.processes) {
        count_prototype(counter, process.prototype_id);
        count_vector_storage(counter, process.input_slots);
        for (const auto& slot : process.input_slots) {
            count_prototype(counter, slot.prototype_id);
        }
        count_vector_storage(counter, process.output_slots);
        for (const auto& slot : process.output_slots) {
            count_prototype(counter, slot.prototype_id);
        }
        count_string(counter, process.interruption_reason);
        count_string(counter, process.condition_function_id);
    }

    count_vector_storage(counter, snapshot.mod_states);
    for (const auto& state : snapshot.mod_states) {
        count_string(counter, state.mod_id);
        count_string(counter, state.state_key);
        count_string(counter, state.encoded_state);
    }

    count_vector_storage(counter, snapshot.missing_prototypes);
    for (const auto& missing : snapshot.missing_prototypes) {
        count_prototype(counter, missing.original_prototype_id);
        count_string(counter, missing.saved_blob);
        count_string(counter, missing.warning);
    }

    count_vector_storage(counter, snapshot.fires);
    for (const auto& fire : snapshot.fires) {
        count_prototype(counter, fire.prototype_id);
    }
}

[[nodiscard]] bool request_metadata_is_valid(const SaveRequest& request) noexcept {
    if (request.database_root.empty()) {
        return false;
    }
    if (!request.slot_metadata_update.has_value()) {
        return true;
    }
    const auto& update = *request.slot_metadata_update;
    return !update.catalog_root.empty() &&
           FileSaveSlotCatalog::is_valid_slot_id(update.slot_id) && update.saved_at_ms != 0 &&
           request.database_root.lexically_normal() ==
               (update.catalog_root / update.slot_id).lexically_normal();
}

} // namespace

SaveSnapshotMemoryEstimate estimate_save_snapshot_memory(const SaveSnapshot& snapshot) noexcept {
    ByteCounter retained;
    retained.add(sizeof(snapshot));
    count_snapshot_payload(retained, snapshot);

    ByteCounter working;
    constexpr std::size_t codec_allowance = 64U * 1024U;
    working.add(codec_allowance);
    working.add_product(retained.value(), 4U);
    return {retained.value(), working.value(), retained.saturated() || working.saturated()};
}

struct SaveScheduler::SharedState {
    explicit SharedState(std::size_t maximum_completed_results)
        : max_completed_results(maximum_completed_results) {}

    void publish(SaveResult result) {
        {
            std::lock_guard lock(mutex);
            // Config validation and retaining active requests until drain guarantee this bound.
            if (mailbox.size() < max_completed_results) {
                mailbox.push_back(std::move(result));
            }
        }
        completion_available.notify_one();
    }

    [[nodiscard]] std::vector<SaveResult> wait_drain(std::chrono::milliseconds timeout,
                                                     std::size_t maximum_results) {
        std::vector<SaveResult> results;
        std::unique_lock lock(mutex);
        static_cast<void>(completion_available.wait_for(lock, timeout,
                                                        [this] { return !mailbox.empty(); }));
        const auto count = std::min(maximum_results, mailbox.size());
        results.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            results.push_back(std::move(mailbox.front()));
            mailbox.pop_front();
        }
        return results;
    }

    [[nodiscard]] std::vector<SaveResult> drain(std::size_t maximum_results) {
        std::vector<SaveResult> results;
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

    std::size_t max_completed_results = 0;
    mutable std::mutex mutex;
    std::condition_variable completion_available;
    std::deque<SaveResult> mailbox;
};

core::Status SaveSchedulerConfig::validate() const {
    if (max_concurrent_requests == 0 || max_completed_results == 0 ||
        max_request_working_bytes == 0 || max_reserved_working_bytes == 0 ||
        first_request_id == 0) {
        return core::Status::failure(
            "save_scheduler.invalid_config",
            "save scheduler limits, memory budgets, and first request id must be nonzero");
    }
    if (max_completed_results < max_concurrent_requests) {
        return core::Status::failure(
            "save_scheduler.invalid_completed_limit",
            "save scheduler completed-result limit must cover every concurrent request");
    }
    if (max_request_working_bytes > max_reserved_working_bytes) {
        return core::Status::failure(
            "save_scheduler.invalid_memory_budget",
            "per-request save reservation cannot exceed the aggregate reservation budget");
    }
    return core::Status::ok();
}

core::Result<std::unique_ptr<SaveScheduler>> SaveScheduler::create(SaveSchedulerConfig config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<SaveScheduler>>::failure(status.error().code,
                                                                     status.error().message);
    }
    jobs::JobSystemDesc jobs_config;
    jobs_config.backend = jobs::JobBackend::thread_pool;
    jobs_config.worker_count = 1;
    jobs_config.max_pending_jobs = static_cast<std::uint32_t>(
        std::min(config.max_concurrent_requests,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    jobs_config.max_completed_results = static_cast<std::uint32_t>(
        std::min(config.max_completed_results,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    auto jobs = jobs::create_job_system(jobs_config);
    if (!jobs) {
        return core::Result<std::unique_ptr<SaveScheduler>>::failure(jobs.error().code,
                                                                     jobs.error().message);
    }
    auto shared = std::make_shared<SharedState>(config.max_completed_results);
    return core::Result<std::unique_ptr<SaveScheduler>>::success(std::unique_ptr<SaveScheduler>(
        new SaveScheduler(config, std::move(jobs).value(), std::move(shared))));
}

SaveScheduler::SaveScheduler(SaveSchedulerConfig config, std::unique_ptr<jobs::IJobSystem> jobs,
                             std::shared_ptr<SharedState> shared_state)
    : config_(config), jobs_(std::move(jobs)), shared_state_(std::move(shared_state)),
      next_request_id_(config.first_request_id) {}

SaveScheduler::~SaveScheduler() {
    shutdown();
}

core::Result<SaveRequestId> SaveScheduler::submit(SaveRequest request) {
    const auto request_received_at = std::chrono::steady_clock::now();
    if (jobs_ == nullptr) {
        ++stats_.rejected_requests;
        return core::Result<SaveRequestId>::failure("save_scheduler.stopped",
                                                    "save scheduler is stopped");
    }
    if (!request_metadata_is_valid(request)) {
        ++stats_.rejected_requests;
        return core::Result<SaveRequestId>::failure("save_scheduler.invalid_request",
                                                    "save request requires a database root");
    }
    const auto memory = estimate_save_snapshot_memory(request.snapshot);
    if (memory.saturated || memory.working_reservation_bytes > config_.max_request_working_bytes) {
        ++stats_.rejected_requests;
        return core::Result<SaveRequestId>::failure(
            "save_scheduler.request_memory_budget_exceeded",
            "save request exceeds the configured per-request working-memory reservation");
    }
    if (active_requests_.size() >= config_.max_concurrent_requests ||
        memory.working_reservation_bytes >
            config_.max_reserved_working_bytes - stats_.reserved_working_bytes) {
        ++stats_.rejected_requests;
        return core::Result<SaveRequestId>::failure(
            "save_scheduler.full",
            "save scheduler reached its request or working-memory reservation budget");
    }
    if (next_request_id_ == 0) {
        ++stats_.rejected_requests;
        return core::Result<SaveRequestId>::failure("save_scheduler.request_id_exhausted",
                                                    "save request identifier range is exhausted");
    }

    const auto request_id = SaveRequestId::from_value(next_request_id_);
    auto shared = shared_state_;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    jobs::JobDesc job;
    job.name = "save_snapshot";
    job.type = "persistence.snapshot";
    job.priority = jobs::JobPriority::normal;
    job.estimated_cost = static_cast<std::uint32_t>(
        std::min(memory.working_reservation_bytes / 1024U + 1U,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    job.work = [request_id, request = std::move(request), memory, shared, request_received_at,
                cancellation](const jobs::JobContext& context) mutable {
        SaveResult result;
        result.request_id = request_id;
        result.reserved_working_bytes = memory.working_reservation_bytes;
        {
            profiling::ScopedCpuTimer total_timer(result.total_worker_ms);
            if (context.cancellation_requested() || cancellation->load(std::memory_order_acquire)) {
                result.state = SaveResultState::cancelled;
            } else {
                try {
                    FileSaveDatabase database(std::move(request.database_root));
                    auto accepted = [&] {
                        profiling::ScopedCpuTimer timer(result.durable_acceptance_ms);
                        return database.journal_snapshot(request.snapshot);
                    }();
                    if (!accepted) {
                        result.state = SaveResultState::failed;
                        result.error_code = accepted.error().code;
                        result.error_message = accepted.error().message;
                    } else {
                        result.state = SaveResultState::succeeded;
                        result.durably_accepted = true;
                        result.request_to_durable_acceptance_ms =
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - request_received_at)
                                .count();
                        result.journal_sequence = accepted.value().sequence;
                        result.encoded_bytes = accepted.value().encoded_bytes;
                        if (request.slot_metadata_update.has_value()) {
                            const auto& update = *request.slot_metadata_update;
                            auto metadata_status =
                                FileSaveSlotCatalog(update.catalog_root)
                                    .mark_saved(update.slot_id, update.saved_at_ms);
                            if (metadata_status) {
                                result.slot_metadata_updated = true;
                            } else {
                                result.metadata_error_code = metadata_status.error().code;
                                result.metadata_error_message = metadata_status.error().message;
                            }
                        }
                        if (request.compact_after_acceptance && !context.cancellation_requested() &&
                            !cancellation->load(std::memory_order_acquire)) {
                            auto compacted = [&] {
                                profiling::ScopedCpuTimer timer(result.compaction_ms);
                                return database.compact_snapshot_journal();
                            }();
                            if (compacted) {
                                result.compacted =
                                    compacted.value().compacted &&
                                    compacted.value().compacted_sequence >= result.journal_sequence;
                            } else {
                                result.compaction_error_code = compacted.error().code;
                                result.compaction_error_message = compacted.error().message;
                            }
                        }
                    }
                } catch (const std::exception& exception) {
                    if (result.durably_accepted) {
                        result.state = SaveResultState::succeeded;
                        result.compaction_error_code = "save_scheduler.compaction_exception";
                        result.compaction_error_message = exception.what();
                    } else {
                        result.state = SaveResultState::failed;
                        result.error_code = "save_scheduler.worker_exception";
                        result.error_message = exception.what();
                    }
                } catch (...) {
                    if (result.durably_accepted) {
                        result.state = SaveResultState::succeeded;
                        result.compaction_error_code = "save_scheduler.compaction_exception";
                        result.compaction_error_message =
                            "save worker threw after durable acceptance";
                    } else {
                        result.state = SaveResultState::failed;
                        result.error_code = "save_scheduler.worker_exception";
                        result.error_message = "save worker threw an unknown exception";
                    }
                }
            }
        }
        shared->publish(std::move(result));
        return core::Status::ok();
    };

    auto submitted = jobs_->submit(std::move(job));
    if (!submitted) {
        ++stats_.rejected_requests;
        refresh_stats();
        return core::Result<SaveRequestId>::failure(submitted.error().code,
                                                    submitted.error().message);
    }
    active_requests_.emplace(request_id,
                             ActiveRequest{submitted.value(), memory.working_reservation_bytes,
                                           std::move(cancellation)});
    stats_.reserved_working_bytes += memory.working_reservation_bytes;
    stats_.reserved_working_bytes_high_water =
        std::max(stats_.reserved_working_bytes_high_water, stats_.reserved_working_bytes);
    ++stats_.submitted_requests;
    if (next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
        next_request_id_ = 0;
    } else {
        ++next_request_id_;
    }
    refresh_stats();
    return core::Result<SaveRequestId>::success(request_id);
}

core::Result<SaveRequestId>
SaveScheduler::submit_checkpoint(std::filesystem::path database_root,
                                 std::size_t working_reservation_bytes) {
    if (jobs_ == nullptr) {
        ++stats_.rejected_requests;
        return core::Result<SaveRequestId>::failure("save_scheduler.stopped",
                                                    "save scheduler is stopped");
    }
    if (database_root.empty()) {
        ++stats_.rejected_requests;
        return core::Result<SaveRequestId>::failure(
            "save_scheduler.invalid_request", "save checkpoint requires a database root");
    }

    if (working_reservation_bytes == 0 ||
        working_reservation_bytes > config_.max_request_working_bytes) {
        ++stats_.rejected_requests;
        return core::Result<SaveRequestId>::failure(
            "save_scheduler.request_memory_budget_exceeded",
            "save checkpoint requires the accepted snapshot's bounded memory reservation");
    }
    const auto memory_reservation = working_reservation_bytes;
    if (active_requests_.size() >= config_.max_concurrent_requests ||
        memory_reservation >
            config_.max_reserved_working_bytes - stats_.reserved_working_bytes) {
        ++stats_.rejected_requests;
        return core::Result<SaveRequestId>::failure(
            "save_scheduler.full",
            "save scheduler reached its request or working-memory reservation budget");
    }
    if (next_request_id_ == 0) {
        ++stats_.rejected_requests;
        return core::Result<SaveRequestId>::failure("save_scheduler.request_id_exhausted",
                                                    "save request identifier range is exhausted");
    }

    const auto request_id = SaveRequestId::from_value(next_request_id_);
    auto shared = shared_state_;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    jobs::JobDesc job;
    job.name = "save_checkpoint";
    job.type = "persistence.checkpoint";
    job.priority = jobs::JobPriority::low;
    job.estimated_cost = static_cast<std::uint32_t>(
        std::min(memory_reservation / 1024U + 1U,
                 static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    job.work = [request_id, database_root = std::move(database_root), memory_reservation, shared,
                cancellation](const jobs::JobContext& context) mutable {
        SaveResult result;
        result.request_id = request_id;
        result.operation = SaveOperationKind::checkpoint;
        result.reserved_working_bytes = memory_reservation;
        {
            profiling::ScopedCpuTimer total_timer(result.total_worker_ms);
            if (context.cancellation_requested() || cancellation->load(std::memory_order_acquire)) {
                result.state = SaveResultState::cancelled;
            } else {
                try {
                    FileSaveDatabase database(std::move(database_root));
                    auto compacted = [&] {
                        profiling::ScopedCpuTimer timer(result.compaction_ms);
                        return database.compact_snapshot_journal();
                    }();
                    if (!compacted) {
                        result.state = SaveResultState::failed;
                        result.error_code = compacted.error().code;
                        result.error_message = compacted.error().message;
                    } else {
                        result.state = SaveResultState::succeeded;
                        result.compacted = compacted.value().compacted;
                        result.journal_sequence = compacted.value().compacted_sequence;
                    }
                } catch (const std::exception& exception) {
                    result.state = SaveResultState::failed;
                    result.error_code = "save_scheduler.checkpoint_exception";
                    result.error_message = exception.what();
                } catch (...) {
                    result.state = SaveResultState::failed;
                    result.error_code = "save_scheduler.checkpoint_exception";
                    result.error_message = "save checkpoint worker threw an unknown exception";
                }
            }
        }
        shared->publish(std::move(result));
        return core::Status::ok();
    };

    auto submitted = jobs_->submit(std::move(job));
    if (!submitted) {
        ++stats_.rejected_requests;
        refresh_stats();
        return core::Result<SaveRequestId>::failure(submitted.error().code,
                                                    submitted.error().message);
    }
    active_requests_.emplace(request_id,
                             ActiveRequest{submitted.value(), memory_reservation,
                                           std::move(cancellation)});
    stats_.reserved_working_bytes += memory_reservation;
    stats_.reserved_working_bytes_high_water =
        std::max(stats_.reserved_working_bytes_high_water, stats_.reserved_working_bytes);
    ++stats_.submitted_requests;
    ++stats_.submitted_checkpoint_requests;
    if (next_request_id_ == std::numeric_limits<std::uint64_t>::max()) {
        next_request_id_ = 0;
    } else {
        ++next_request_id_;
    }
    refresh_stats();
    return core::Result<SaveRequestId>::success(request_id);
}

std::vector<SaveResult> SaveScheduler::drain_completed(std::size_t maximum_results) {
    if (jobs_ != nullptr) {
        static_cast<void>(jobs_->drain_completed());
    }
    auto results = shared_state_->drain(maximum_results);
    account_completed(results);
    return results;
}

std::vector<SaveResult>
SaveScheduler::wait_for_completed(std::chrono::milliseconds timeout,
                                  std::size_t maximum_results) {
    auto results = shared_state_->wait_drain(timeout, maximum_results);
    if (jobs_ != nullptr) {
        static_cast<void>(jobs_->drain_completed());
    }
    account_completed(results);
    return results;
}

void SaveScheduler::account_completed(std::span<const SaveResult> results) noexcept {
    for (const auto& result : results) {
        const auto active = active_requests_.find(result.request_id);
        if (active != active_requests_.end()) {
            stats_.reserved_working_bytes -= active->second.reserved_working_bytes;
            active_requests_.erase(active);
        }
        ++stats_.completed_requests;
        if (result.operation == SaveOperationKind::checkpoint) {
            ++stats_.completed_checkpoint_requests;
        }
        if (result.state == SaveResultState::cancelled) {
            ++stats_.cancelled_requests;
        } else if (result.state == SaveResultState::failed) {
            ++stats_.failed_requests;
        }
        if (result.durably_accepted) {
            ++stats_.durably_accepted_requests;
        }
        if (result.compacted) {
            ++stats_.compacted_requests;
        }
        if (!result.metadata_error_code.empty()) {
            ++stats_.metadata_update_failures;
        }
    }
    refresh_stats();
}

core::Status SaveScheduler::cancel(SaveRequestId request_id) noexcept {
    if (jobs_ == nullptr) {
        return core::Status::failure("save_scheduler.stopped", "save scheduler is stopped");
    }
    const auto found = active_requests_.find(request_id);
    if (found == active_requests_.end()) {
        return core::Status::failure("save_scheduler.request_missing",
                                     "save request is not active");
    }
    found->second.cancellation->store(true, std::memory_order_release);
    return core::Status::ok();
}

void SaveScheduler::cancel_all() noexcept {
    if (jobs_ == nullptr) {
        return;
    }
    for (const auto& [_, request] : active_requests_) {
        request.cancellation->store(true, std::memory_order_release);
    }
}

void SaveScheduler::shutdown() noexcept {
    if (jobs_ == nullptr) {
        return;
    }
    cancel_all();
    jobs_.reset();
    static_cast<void>(shared_state_->drain(static_cast<std::size_t>(-1)));
    active_requests_.clear();
    stats_.reserved_working_bytes = 0;
    refresh_stats();
}

bool SaveScheduler::has_capacity() const noexcept {
    return jobs_ != nullptr && active_requests_.size() < config_.max_concurrent_requests &&
           stats_.reserved_working_bytes < config_.max_reserved_working_bytes;
}

bool SaveScheduler::has_in_flight() const noexcept {
    return !active_requests_.empty();
}

const SaveSchedulerStats& SaveScheduler::stats() noexcept {
    refresh_stats();
    return stats_;
}

void SaveScheduler::refresh_stats() noexcept {
    stats_.in_flight_requests = active_requests_.size();
    stats_.completed_mailbox_count = shared_state_->size();
    stats_.oldest_queued_request_age_us =
        jobs_ == nullptr ? 0 : jobs_->stats().oldest_queued_job_age_us;
}

const char* save_result_state_name(SaveResultState state) noexcept {
    switch (state) {
    case SaveResultState::succeeded:
        return "succeeded";
    case SaveResultState::failed:
        return "failed";
    case SaveResultState::cancelled:
        return "cancelled";
    }
    return "unknown";
}

const char* save_operation_kind_name(SaveOperationKind operation) noexcept {
    switch (operation) {
    case SaveOperationKind::snapshot:
        return "snapshot";
    case SaveOperationKind::checkpoint:
        return "checkpoint";
    }
    return "unknown";
}

} // namespace heartstead::save
