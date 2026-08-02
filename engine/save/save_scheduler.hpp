#pragma once

#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/save/save_database.hpp"
#include "engine/save/save_snapshot.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace heartstead::save {

struct SaveRequestIdTag;
using SaveRequestId = core::StrongU64Id<SaveRequestIdTag>;

struct SaveSnapshotMemoryEstimate {
    std::size_t retained_bytes = 0;
    std::size_t working_reservation_bytes = 0;
    bool saturated = false;
};

// Counts retained container/string capacity and reserves four times that measured input plus a
// fixed codec allowance. The multiplier covers the request snapshot, encoded journal buffers,
// recovery decode, and checkpoint encoding that can coexist during one worker operation.
[[nodiscard]] SaveSnapshotMemoryEstimate
estimate_save_snapshot_memory(const SaveSnapshot& snapshot) noexcept;

struct SaveRequest {
    SaveRequest() = default;
    SaveRequest(std::filesystem::path root, SaveSnapshot requested_snapshot,
                bool compact = true)
        : database_root(std::move(root)), snapshot(std::move(requested_snapshot)),
          compact_after_acceptance(compact) {}

    std::filesystem::path database_root;
    SaveSnapshot snapshot;
    bool compact_after_acceptance = true;
    struct SlotMetadataUpdate {
        std::filesystem::path catalog_root;
        std::string slot_id;
        std::uint64_t saved_at_ms = 0;
    };
    std::optional<SlotMetadataUpdate> slot_metadata_update;
};

enum class SaveResultState : std::uint8_t {
    succeeded,
    failed,
    cancelled,
};

struct SaveResult {
    SaveRequestId request_id;
    SaveResultState state = SaveResultState::failed;
    bool durably_accepted = false;
    bool compacted = false;
    bool slot_metadata_updated = false;
    std::uint64_t journal_sequence = 0;
    std::size_t encoded_bytes = 0;
    std::size_t reserved_working_bytes = 0;
    double durable_acceptance_ms = 0.0;
    double compaction_ms = 0.0;
    double total_worker_ms = 0.0;
    std::string error_code;
    std::string error_message;
    std::string compaction_error_code;
    std::string compaction_error_message;
    std::string metadata_error_code;
    std::string metadata_error_message;
};

struct SaveSchedulerConfig {
    std::size_t max_concurrent_requests = 2;
    std::size_t max_completed_results = 4;
    std::size_t max_request_working_bytes = 768U * 1024U * 1024U;
    std::size_t max_reserved_working_bytes = 1024U * 1024U * 1024U;
    std::uint64_t first_request_id = 1;

    [[nodiscard]] core::Status validate() const;
};

struct SaveSchedulerStats {
    std::size_t in_flight_requests = 0;
    std::size_t completed_mailbox_count = 0;
    std::size_t reserved_working_bytes = 0;
    std::size_t reserved_working_bytes_high_water = 0;
    std::uint64_t submitted_requests = 0;
    std::uint64_t completed_requests = 0;
    std::uint64_t durably_accepted_requests = 0;
    std::uint64_t compacted_requests = 0;
    std::uint64_t failed_requests = 0;
    std::uint64_t metadata_update_failures = 0;
    std::uint64_t cancelled_requests = 0;
    std::uint64_t rejected_requests = 0;
    std::uint64_t oldest_queued_request_age_us = 0;
};

class SaveScheduler {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<SaveScheduler>>
    create(SaveSchedulerConfig config = {});

    ~SaveScheduler();

    SaveScheduler(const SaveScheduler&) = delete;
    SaveScheduler& operator=(const SaveScheduler&) = delete;

    [[nodiscard]] core::Result<SaveRequestId> submit(SaveRequest request);
    [[nodiscard]] std::vector<SaveResult>
    drain_completed(std::size_t maximum_results = static_cast<std::size_t>(-1));
    [[nodiscard]] std::vector<SaveResult>
    wait_for_completed(std::chrono::milliseconds timeout,
                       std::size_t maximum_results = static_cast<std::size_t>(-1));
    [[nodiscard]] core::Status cancel(SaveRequestId request_id) noexcept;
    void cancel_all() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool has_capacity() const noexcept;
    [[nodiscard]] bool has_in_flight() const noexcept;
    [[nodiscard]] const SaveSchedulerStats& stats() noexcept;

  private:
    struct SharedState;
    struct ActiveRequest {
        jobs::JobId job_id;
        std::size_t reserved_working_bytes = 0;
        std::shared_ptr<std::atomic_bool> cancellation;
    };

    SaveScheduler(SaveSchedulerConfig config, std::unique_ptr<jobs::IJobSystem> jobs,
                  std::shared_ptr<SharedState> shared_state);

    void refresh_stats() noexcept;
    void account_completed(std::span<const SaveResult> results) noexcept;

    SaveSchedulerConfig config_;
    std::unique_ptr<jobs::IJobSystem> jobs_;
    std::shared_ptr<SharedState> shared_state_;
    std::map<SaveRequestId, ActiveRequest> active_requests_;
    std::uint64_t next_request_id_ = 1;
    SaveSchedulerStats stats_;
};

[[nodiscard]] const char* save_result_state_name(SaveResultState state) noexcept;

} // namespace heartstead::save
