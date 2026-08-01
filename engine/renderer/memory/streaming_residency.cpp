#include "engine/renderer/memory/streaming_residency.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <tuple>
#include <utility>

namespace heartstead::renderer {

namespace {

[[nodiscard]] jobs::JobPriority job_priority(float priority) noexcept {
    return priority >= 1.0F ? jobs::JobPriority::high
                            : (priority > 0.0F ? jobs::JobPriority::normal
                                               : jobs::JobPriority::low);
}

[[nodiscard]] bool valid_request(const ResidencyRequest& request) noexcept {
    return !request.id.empty() && std::isfinite(request.priority) && request.priority >= 0.0F;
}

} // namespace

core::Result<std::unique_ptr<StreamingResidencyManager>>
StreamingResidencyManager::create(StreamingResidencyConfig config,
                                  ResidencyLoadFunction loader) {
    if (!loader) {
        return core::Result<std::unique_ptr<StreamingResidencyManager>>::failure(
            "renderer.residency_missing_loader", "streaming residency requires a load callback");
    }
    if (config.worker_count == 0 || config.maximum_in_flight_loads == 0 ||
        config.upload_budget_bytes == 0 || config.upload_budget_resources == 0 ||
        config.resident_budget_bytes == 0 ||
        !std::isfinite(config.reported_heap_budget_fraction) ||
        config.reported_heap_budget_fraction <= 0.0F ||
        config.reported_heap_budget_fraction > 1.0F) {
        return core::Result<std::unique_ptr<StreamingResidencyManager>>::failure(
            "renderer.invalid_residency_config",
            "streaming residency budgets, worker counts, and heap fraction must be positive");
    }
    auto jobs = jobs::create_job_system(
        {config.job_backend, config.worker_count,
         std::max(config.maximum_in_flight_loads * 4U, 64U), 1});
    if (!jobs) {
        return core::Result<std::unique_ptr<StreamingResidencyManager>>::failure(
            jobs.error().code, jobs.error().message);
    }
    return core::Result<std::unique_ptr<StreamingResidencyManager>>::success(
        std::unique_ptr<StreamingResidencyManager>(new StreamingResidencyManager(
            std::move(config), std::move(loader), std::move(jobs.value()))));
}

StreamingResidencyManager::StreamingResidencyManager(StreamingResidencyConfig config,
                                                     ResidencyLoadFunction loader,
                                                     std::unique_ptr<jobs::IJobSystem> jobs)
    : config_(std::move(config)), loader_(std::move(loader)), jobs_(std::move(jobs)) {}

StreamingResidencyManager::~StreamingResidencyManager() {
    if (jobs_ != nullptr) {
        for (auto& [id, record] : records_) {
            static_cast<void>(id);
            if (record.cancellation != nullptr) {
                record.cancellation->store(true, std::memory_order_relaxed);
            }
        }
        jobs_.reset();
    }
}

void StreamingResidencyManager::begin_frame(std::uint64_t frame_index) noexcept {
    stats_.frame_index = frame_index;
    stats_.uploaded_bytes_this_frame = 0;
    stats_.uploaded_resources_this_frame = 0;
}

core::Status StreamingResidencyManager::request(ResidencyRequest request) {
    if (shutdown_) {
        return core::Status::failure("renderer.residency_shutdown",
                                     "cannot request a resource after residency shutdown");
    }
    if (!valid_request(request)) {
        return core::Status::failure("renderer.invalid_residency_request",
                                     "residency request id and non-negative finite priority are required");
    }

    auto [iterator, inserted] = records_.try_emplace(request.id);
    auto& record = iterator->second;
    if (inserted) {
        record.request = std::move(request);
        record.state = ResidencyState::queued;
        record.last_requested_frame = stats_.frame_index;
        refresh_stats();
        return core::Status::ok();
    }

    const auto needs_upgrade = !record.resident_detail_level.has_value() ||
                               request.detail_level < *record.resident_detail_level;
    const auto request_changed = request.detail_level != record.request.detail_level ||
                                 request.resource_class != record.request.resource_class;
    record.request = std::move(request);
    record.requested = true;
    record.last_requested_frame = stats_.frame_index;
    if (request_changed && record.load_in_flight) {
        record.cancellation->store(true, std::memory_order_relaxed);
        ++record.generation;
        record.load_in_flight = false;
        record.pending_payload.reset();
    }
    if (needs_upgrade) {
        record.state = ResidencyState::queued;
    } else if (record.resident.has_value()) {
        record.state = ResidencyState::resident;
    }
    refresh_stats();
    return core::Status::ok();
}

void StreamingResidencyManager::cancel(std::string_view id) {
    const auto found = records_.find(std::string(id));
    if (found == records_.end()) {
        return;
    }
    auto& record = found->second;
    record.requested = false;
    record.state = ResidencyState::cancelled;
    record.pending_payload.reset();
    if (record.cancellation != nullptr) {
        record.cancellation->store(true, std::memory_order_relaxed);
    }
    ++record.generation;
    record.load_in_flight = false;
    ++stats_.cancelled_loads;
    refresh_stats();
}

void StreamingResidencyManager::set_reported_heap_budget(std::size_t bytes) noexcept {
    reported_heap_budget_bytes_ = bytes;
}

core::Status StreamingResidencyManager::process(const ResidencyUploadFunction& uploader,
                                                const ResidencyReleaseFunction& releaser) {
    if (shutdown_) {
        return core::Status::failure("renderer.residency_shutdown",
                                     "cannot process residency after shutdown");
    }
    if (!uploader || !releaser) {
        return core::Status::failure("renderer.residency_missing_gpu_callback",
                                     "residency processing requires upload and release callbacks");
    }
    schedule_loads();
    harvest_loads();
    auto upload_status = upload_ready(uploader, releaser);
    enforce_budget(releaser);
    refresh_stats();
    return upload_status;
}

void StreamingResidencyManager::schedule_loads() {
    const auto active_jobs = jobs_->submitted_count() - jobs_->completed_count();
    if (active_jobs >= config_.maximum_in_flight_loads) {
        return;
    }
    std::vector<Record*> queued;
    for (auto& [id, record] : records_) {
        static_cast<void>(id);
        if (record.requested && !record.load_in_flight && record.state == ResidencyState::queued) {
            queued.push_back(&record);
        }
    }
    std::ranges::sort(queued, [](const Record* left, const Record* right) {
        return std::tuple{-left->request.priority, left->request.detail_level, left->request.id} <
               std::tuple{-right->request.priority, right->request.detail_level, right->request.id};
    });

    auto remaining = static_cast<std::size_t>(config_.maximum_in_flight_loads - active_jobs);
    for (auto* record : queued) {
        if (remaining == 0) {
            break;
        }
        const auto request_copy = record->request;
        const auto generation = record->generation;
        auto cancellation = std::make_shared<std::atomic_bool>(false);
        record->cancellation = cancellation;
        const auto completions = completions_;
        const auto loader = loader_;
        auto submitted = jobs_->submit(
            {"residency.load." + request_copy.id, job_priority(request_copy.priority),
             [request_copy, generation, cancellation, completions,
              loader](const jobs::JobContext&) mutable {
                 CompletedLoad completed;
                 completed.id = request_copy.id;
                 completed.generation = generation;
                 if (cancellation->load(std::memory_order_relaxed)) {
                     completed.cancelled = true;
                 } else {
                     auto payload = loader(request_copy, *cancellation);
                     if (cancellation->load(std::memory_order_relaxed)) {
                         completed.cancelled = true;
                     } else if (payload) {
                         completed.payload = std::move(payload.value());
                     } else {
                         completed.error_code = payload.error().code;
                         completed.error_message = payload.error().message;
                     }
                 }
                 std::scoped_lock lock(completions->mutex);
                 completions->values.push_back(std::move(completed));
                 return core::Status::ok();
             }});
        if (!submitted) {
            record->state = ResidencyState::failed;
            record->error_code = submitted.error().code;
            record->error_message = submitted.error().message;
            ++stats_.failed_loads;
            continue;
        }
        record->load_in_flight = true;
        record->state = ResidencyState::loading;
        --remaining;
    }
}

void StreamingResidencyManager::harvest_loads() {
    static_cast<void>(jobs_->drain_completed());
    std::vector<CompletedLoad> completed;
    {
        std::scoped_lock lock(completions_->mutex);
        completed.swap(completions_->values);
    }
    std::ranges::sort(completed, {}, &CompletedLoad::id);
    for (auto& load : completed) {
        const auto found = records_.find(load.id);
        if (found == records_.end() || found->second.generation != load.generation ||
            !found->second.requested) {
            ++stats_.stale_loads_discarded;
            continue;
        }
        auto& record = found->second;
        record.load_in_flight = false;
        if (load.cancelled) {
            record.state = record.resident.has_value() ? ResidencyState::resident
                                                       : ResidencyState::cancelled;
            ++stats_.cancelled_loads;
        } else if (!load.payload.has_value()) {
            record.state = ResidencyState::failed;
            record.error_code = std::move(load.error_code);
            record.error_message = std::move(load.error_message);
            ++stats_.failed_loads;
        } else if (load.payload->id != record.request.id ||
                   load.payload->resource_class != record.request.resource_class ||
                   load.payload->detail_level != record.request.detail_level) {
            record.state = ResidencyState::failed;
            record.error_code = "renderer.residency_payload_mismatch";
            record.error_message = "loaded residency payload does not match its request";
            ++stats_.failed_loads;
        } else {
            record.pending_payload = std::move(load.payload);
            record.state = ResidencyState::upload_pending;
        }
    }
}

core::Status StreamingResidencyManager::upload_ready(
    const ResidencyUploadFunction& uploader, const ResidencyReleaseFunction& releaser) {
    std::vector<Record*> ready;
    for (auto& [id, record] : records_) {
        static_cast<void>(id);
        if (record.requested && record.pending_payload.has_value()) {
            ready.push_back(&record);
        }
    }
    std::ranges::sort(ready, [](const Record* left, const Record* right) {
        return std::tuple{-left->request.priority, left->request.detail_level, left->request.id} <
               std::tuple{-right->request.priority, right->request.detail_level, right->request.id};
    });

    core::Status status = core::Status::ok();
    for (auto* record : ready) {
        const auto payload_bytes = record->pending_payload->bytes.size();
        const auto exceeds_bytes = stats_.uploaded_bytes_this_frame + payload_bytes >
                                   config_.upload_budget_bytes;
        const auto exceeds_count = stats_.uploaded_resources_this_frame >=
                                   config_.upload_budget_resources;
        if ((exceeds_bytes && stats_.uploaded_resources_this_frame > 0) || exceeds_count) {
            continue;
        }

        const auto detail_level = record->pending_payload->detail_level;
        auto uploaded = uploader(std::move(*record->pending_payload));
        record->pending_payload.reset();
        if (!uploaded || !uploaded.value().handle.is_valid()) {
            record->state = ResidencyState::failed;
            record->error_code = uploaded ? "renderer.residency_invalid_gpu_resource"
                                          : uploaded.error().code;
            record->error_message = uploaded ? "residency upload returned an invalid handle"
                                             : uploaded.error().message;
            ++stats_.failed_uploads;
            if (status) {
                status = core::Status::failure(record->error_code, record->error_message);
            }
            continue;
        }
        if (record->resident.has_value()) {
            releaser(*record->resident);
        }
        record->resident = uploaded.value();
        record->resident_detail_level = detail_level;
        record->state = ResidencyState::resident;
        stats_.uploaded_bytes_this_frame += payload_bytes;
        ++stats_.uploaded_resources_this_frame;
    }
    return status;
}

void StreamingResidencyManager::enforce_budget(const ResidencyReleaseFunction& releaser) {
    const auto release_record = [this, &releaser](Record& record) {
        if (!record.resident.has_value()) {
            return;
        }
        releaser(*record.resident);
        record.resident.reset();
        record.resident_detail_level.reset();
        record.state = record.requested ? ResidencyState::queued : ResidencyState::fallback;
        ++stats_.evicted_resources;
    };

    for (auto& [id, record] : records_) {
        static_cast<void>(id);
        if (!record.requested) {
            release_record(record);
        }
    }

    std::size_t resident_bytes = 0;
    for (const auto& [id, record] : records_) {
        static_cast<void>(id);
        resident_bytes += record.resident.has_value() ? record.resident->gpu_bytes : 0;
    }
    const auto budget = effective_resident_budget();
    if (resident_bytes <= budget) {
        return;
    }

    std::vector<Record*> candidates;
    for (auto& [id, record] : records_) {
        static_cast<void>(id);
        if (record.resident.has_value() && !record.request.pinned) {
            candidates.push_back(&record);
        }
    }
    std::ranges::sort(candidates, [](const Record* left, const Record* right) {
        return std::tuple{left->last_requested_frame, left->request.priority, left->request.id} <
               std::tuple{right->last_requested_frame, right->request.priority, right->request.id};
    });
    for (auto* record : candidates) {
        if (resident_bytes <= budget) {
            break;
        }
        resident_bytes -= record->resident->gpu_bytes;
        release_record(*record);
    }
}

std::size_t StreamingResidencyManager::effective_resident_budget() const noexcept {
    if (reported_heap_budget_bytes_ == 0) {
        return config_.resident_budget_bytes;
    }
    const auto heap_limit = static_cast<std::size_t>(
        static_cast<double>(reported_heap_budget_bytes_) *
        static_cast<double>(config_.reported_heap_budget_fraction));
    return std::min(config_.resident_budget_bytes, heap_limit);
}

rhi::RenderResourceHandle StreamingResidencyManager::fallback_for(
    ResidencyResourceClass resource_class) const noexcept {
    return resource_class == ResidencyResourceClass::texture ? config_.texture_fallback
                                                              : config_.mesh_fallback;
}

rhi::RenderResourceHandle StreamingResidencyManager::resolve(
    std::string_view id, ResidencyResourceClass resource_class) const {
    const auto found = records_.find(std::string(id));
    return found != records_.end() && found->second.resident.has_value()
               ? found->second.resident->handle
               : fallback_for(resource_class);
}

ResidencyState StreamingResidencyManager::state(std::string_view id) const noexcept {
    const auto found = records_.find(std::string(id));
    return found == records_.end() ? ResidencyState::fallback : found->second.state;
}

std::vector<ResidencyRecordView> StreamingResidencyManager::records() const {
    std::vector<ResidencyRecordView> result;
    result.reserve(records_.size());
    for (const auto& [id, record] : records_) {
        result.push_back({id, record.state, record.request.detail_level,
                          record.resident_detail_level, record.request.priority,
                          record.resident.has_value() ? record.resident->gpu_bytes : 0,
                          record.last_requested_frame});
    }
    std::ranges::sort(result, {}, &ResidencyRecordView::id);
    return result;
}

const StreamingResidencyStats& StreamingResidencyManager::stats() const noexcept {
    return stats_;
}

void StreamingResidencyManager::refresh_stats() {
    stats_.tracked_resources = records_.size();
    stats_.queued_resources = 0;
    stats_.in_flight_loads = 0;
    stats_.upload_pending_resources = 0;
    stats_.resident_resources = 0;
    stats_.failed_resources = 0;
    stats_.resident_bytes = 0;
    stats_.pending_upload_bytes = 0;
    for (const auto& [id, record] : records_) {
        static_cast<void>(id);
        stats_.queued_resources += record.state == ResidencyState::queued ? 1U : 0U;
        stats_.in_flight_loads += record.load_in_flight ? 1U : 0U;
        stats_.upload_pending_resources += record.pending_payload.has_value() ? 1U : 0U;
        stats_.resident_resources += record.resident.has_value() ? 1U : 0U;
        stats_.failed_resources += record.state == ResidencyState::failed ? 1U : 0U;
        stats_.resident_bytes += record.resident.has_value() ? record.resident->gpu_bytes : 0U;
        stats_.pending_upload_bytes +=
            record.pending_payload.has_value() ? record.pending_payload->bytes.size() : 0U;
    }
}

void StreamingResidencyManager::shutdown(const ResidencyReleaseFunction& releaser) {
    if (shutdown_) {
        return;
    }
    for (auto& [id, record] : records_) {
        static_cast<void>(id);
        if (record.cancellation != nullptr) {
            record.cancellation->store(true, std::memory_order_relaxed);
        }
    }
    jobs_.reset();
    for (auto& [id, record] : records_) {
        static_cast<void>(id);
        if (record.resident.has_value()) {
            releaser(*record.resident);
        }
    }
    records_.clear();
    shutdown_ = true;
    refresh_stats();
}

} // namespace heartstead::renderer
