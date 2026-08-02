#pragma once

#include "engine/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>

namespace heartstead::save {

struct SaveCheckpointRetryPolicy {
    std::int64_t initial_delay_ms = 250;
    std::int64_t maximum_delay_ms = 30'000;
    std::uint32_t max_attempts = 8;
    std::size_t max_pending_checkpoints = 8;

    [[nodiscard]] core::Status validate() const;
};

struct SaveCheckpointRetryCandidate {
    std::filesystem::path database_root;
    std::size_t working_reservation_bytes = 0;
    std::uint32_t attempt_number = 0;
};

enum class SaveCheckpointRetryOutcome : std::uint8_t {
    completed,
    retryable_failure,
    terminal_failure,
};

enum class SaveCheckpointRetryDisposition : std::uint8_t {
    completed,
    scheduled,
    exhausted,
    terminal_failure,
};

struct SaveCheckpointRetryStats {
    std::size_t pending_checkpoints = 0;
    std::size_t in_flight_checkpoints = 0;
    std::uint64_t deferred_events = 0;
    std::uint64_t submitted_attempts = 0;
    std::uint64_t retryable_failures = 0;
    std::uint64_t completed_checkpoints = 0;
    std::uint64_t exhausted_checkpoints = 0;
    std::uint64_t terminal_failures = 0;
};

// Application-owned, owner-thread retry state. It never sleeps or performs I/O: the caller asks
// for at most one due candidate, submits it to the save worker, and reports the exact outcome.
class SaveCheckpointRetryQueue {
  public:
    explicit SaveCheckpointRetryQueue(SaveCheckpointRetryPolicy policy = {});

    [[nodiscard]] core::Status defer(std::filesystem::path database_root,
                                     std::size_t working_reservation_bytes, std::int64_t now_ms);
    [[nodiscard]] std::optional<SaveCheckpointRetryCandidate> next_due(std::int64_t now_ms) const;
    [[nodiscard]] core::Status note_submitted(const std::filesystem::path& database_root,
                                              std::int64_t now_ms);
    [[nodiscard]] core::Result<SaveCheckpointRetryDisposition>
    note_outcome(const std::filesystem::path& database_root, SaveCheckpointRetryOutcome outcome,
                 std::int64_t now_ms);
    void note_completed_externally(const std::filesystem::path& database_root);

    [[nodiscard]] SaveCheckpointRetryStats stats() const noexcept;
    [[nodiscard]] bool contains(const std::filesystem::path& database_root) const;

  private:
    struct Entry {
        std::int64_t next_attempt_at_ms = 0;
        std::int64_t last_event_at_ms = 0;
        std::size_t working_reservation_bytes = 0;
        std::uint32_t submitted_attempts = 0;
        bool in_flight = false;
    };

    [[nodiscard]] std::int64_t retry_delay_ms(std::uint32_t submitted_attempts) const noexcept;
    [[nodiscard]] static std::filesystem::path
    normalized_root(const std::filesystem::path& database_root);

    SaveCheckpointRetryPolicy policy_;
    std::map<std::filesystem::path, Entry> entries_;
    SaveCheckpointRetryStats lifetime_;
};

[[nodiscard]] const char*
save_checkpoint_retry_disposition_name(SaveCheckpointRetryDisposition disposition) noexcept;

} // namespace heartstead::save
