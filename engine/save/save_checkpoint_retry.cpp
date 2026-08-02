#include "engine/save/save_checkpoint_retry.hpp"

#include <algorithm>
#include <limits>

namespace heartstead::save {

namespace {

[[nodiscard]] std::int64_t saturated_add(std::int64_t left, std::int64_t right) noexcept {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    return left > maximum - right ? maximum : left + right;
}

} // namespace

core::Status SaveCheckpointRetryPolicy::validate() const {
    if (initial_delay_ms <= 0 || maximum_delay_ms <= 0 || max_attempts == 0 ||
        max_pending_checkpoints == 0) {
        return core::Status::failure(
            "save_checkpoint_retry.invalid_policy",
            "checkpoint retry delays, attempt limit, and pending limit must be positive");
    }
    if (initial_delay_ms > maximum_delay_ms) {
        return core::Status::failure(
            "save_checkpoint_retry.invalid_delay_order",
            "checkpoint retry initial delay cannot exceed the maximum delay");
    }
    return core::Status::ok();
}

SaveCheckpointRetryQueue::SaveCheckpointRetryQueue(SaveCheckpointRetryPolicy policy)
    : policy_(policy) {}

core::Status SaveCheckpointRetryQueue::defer(std::filesystem::path database_root,
                                             std::size_t working_reservation_bytes,
                                             std::int64_t now_ms) {
    auto policy_status = policy_.validate();
    if (!policy_status) {
        return policy_status;
    }
    auto root = normalized_root(database_root);
    if (root.empty()) {
        return core::Status::failure("save_checkpoint_retry.invalid_root",
                                     "deferred checkpoint requires a database root");
    }
    if (now_ms < 0) {
        return core::Status::failure("save_checkpoint_retry.invalid_time",
                                     "checkpoint retry time must be nonnegative");
    }
    if (working_reservation_bytes == 0) {
        return core::Status::failure(
            "save_checkpoint_retry.invalid_memory_reservation",
            "deferred checkpoint requires the accepted snapshot's memory reservation");
    }

    const auto found = entries_.find(root);
    if (found != entries_.end()) {
        if (found->second.in_flight) {
            return core::Status::failure(
                "save_checkpoint_retry.in_flight",
                "cannot replace checkpoint retry state while an attempt is in flight");
        }
        if (now_ms < found->second.last_event_at_ms) {
            return core::Status::failure("save_checkpoint_retry.time_reversed",
                                         "checkpoint retry time cannot move backward");
        }
        found->second = {saturated_add(now_ms, policy_.initial_delay_ms), now_ms,
                         working_reservation_bytes, 0, false};
    } else {
        if (entries_.size() >= policy_.max_pending_checkpoints) {
            return core::Status::failure(
                "save_checkpoint_retry.full",
                "deferred checkpoint queue reached its configured root limit");
        }
        entries_.emplace(std::move(root), Entry{saturated_add(now_ms, policy_.initial_delay_ms),
                                                now_ms, working_reservation_bytes, 0, false});
    }
    ++lifetime_.deferred_events;
    return core::Status::ok();
}

std::optional<SaveCheckpointRetryCandidate>
SaveCheckpointRetryQueue::next_due(std::int64_t now_ms) const {
    if (now_ms < 0) {
        return std::nullopt;
    }
    auto selected = entries_.end();
    for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
        const auto& entry = iterator->second;
        if (entry.in_flight || entry.submitted_attempts >= policy_.max_attempts ||
            now_ms < entry.next_attempt_at_ms) {
            continue;
        }
        if (selected == entries_.end() ||
            entry.next_attempt_at_ms < selected->second.next_attempt_at_ms ||
            (entry.next_attempt_at_ms == selected->second.next_attempt_at_ms &&
             iterator->first < selected->first)) {
            selected = iterator;
        }
    }
    if (selected == entries_.end()) {
        return std::nullopt;
    }
    return SaveCheckpointRetryCandidate{selected->first, selected->second.working_reservation_bytes,
                                        selected->second.submitted_attempts + 1U};
}

core::Status SaveCheckpointRetryQueue::note_submitted(const std::filesystem::path& database_root,
                                                      std::int64_t now_ms) {
    const auto root = normalized_root(database_root);
    const auto found = entries_.find(root);
    if (found == entries_.end()) {
        return core::Status::failure("save_checkpoint_retry.unknown_root",
                                     "checkpoint submission has no deferred root");
    }
    auto& entry = found->second;
    if (now_ms < entry.last_event_at_ms) {
        return core::Status::failure("save_checkpoint_retry.time_reversed",
                                     "checkpoint retry time cannot move backward");
    }
    if (entry.in_flight) {
        return core::Status::failure("save_checkpoint_retry.already_in_flight",
                                     "checkpoint root already has an in-flight attempt");
    }
    if (entry.submitted_attempts >= policy_.max_attempts) {
        return core::Status::failure("save_checkpoint_retry.exhausted",
                                     "checkpoint root exhausted its retry attempts");
    }
    if (now_ms < entry.next_attempt_at_ms) {
        return core::Status::failure("save_checkpoint_retry.not_due",
                                     "checkpoint retry delay has not elapsed");
    }
    entry.in_flight = true;
    entry.last_event_at_ms = now_ms;
    ++entry.submitted_attempts;
    ++lifetime_.submitted_attempts;
    return core::Status::ok();
}

core::Result<SaveCheckpointRetryDisposition>
SaveCheckpointRetryQueue::note_outcome(const std::filesystem::path& database_root,
                                       SaveCheckpointRetryOutcome outcome, std::int64_t now_ms) {
    const auto root = normalized_root(database_root);
    const auto found = entries_.find(root);
    if (found == entries_.end()) {
        return core::Result<SaveCheckpointRetryDisposition>::failure(
            "save_checkpoint_retry.unknown_root", "checkpoint outcome has no deferred root");
    }
    auto& entry = found->second;
    if (now_ms < entry.last_event_at_ms) {
        return core::Result<SaveCheckpointRetryDisposition>::failure(
            "save_checkpoint_retry.time_reversed", "checkpoint retry time cannot move backward");
    }
    if (!entry.in_flight) {
        return core::Result<SaveCheckpointRetryDisposition>::failure(
            "save_checkpoint_retry.not_in_flight",
            "checkpoint outcome requires an in-flight attempt");
    }

    switch (outcome) {
    case SaveCheckpointRetryOutcome::completed:
        entries_.erase(found);
        ++lifetime_.completed_checkpoints;
        return core::Result<SaveCheckpointRetryDisposition>::success(
            SaveCheckpointRetryDisposition::completed);
    case SaveCheckpointRetryOutcome::terminal_failure:
        entries_.erase(found);
        ++lifetime_.terminal_failures;
        return core::Result<SaveCheckpointRetryDisposition>::success(
            SaveCheckpointRetryDisposition::terminal_failure);
    case SaveCheckpointRetryOutcome::retryable_failure:
        ++lifetime_.retryable_failures;
        entry.in_flight = false;
        entry.last_event_at_ms = now_ms;
        if (entry.submitted_attempts >= policy_.max_attempts) {
            entries_.erase(found);
            ++lifetime_.exhausted_checkpoints;
            return core::Result<SaveCheckpointRetryDisposition>::success(
                SaveCheckpointRetryDisposition::exhausted);
        }
        entry.next_attempt_at_ms = saturated_add(now_ms, retry_delay_ms(entry.submitted_attempts));
        return core::Result<SaveCheckpointRetryDisposition>::success(
            SaveCheckpointRetryDisposition::scheduled);
    }
    return core::Result<SaveCheckpointRetryDisposition>::failure(
        "save_checkpoint_retry.invalid_outcome", "checkpoint retry outcome is invalid");
}

void SaveCheckpointRetryQueue::note_completed_externally(
    const std::filesystem::path& database_root) {
    entries_.erase(normalized_root(database_root));
}

SaveCheckpointRetryStats SaveCheckpointRetryQueue::stats() const noexcept {
    auto result = lifetime_;
    result.pending_checkpoints = entries_.size();
    result.in_flight_checkpoints = static_cast<std::size_t>(
        std::ranges::count_if(entries_, [](const auto& pair) { return pair.second.in_flight; }));
    return result;
}

bool SaveCheckpointRetryQueue::contains(const std::filesystem::path& database_root) const {
    return entries_.contains(normalized_root(database_root));
}

std::int64_t
SaveCheckpointRetryQueue::retry_delay_ms(std::uint32_t submitted_attempts) const noexcept {
    auto delay = policy_.initial_delay_ms;
    for (std::uint32_t index = 0; index < submitted_attempts && delay < policy_.maximum_delay_ms;
         ++index) {
        delay =
            std::min(policy_.maximum_delay_ms,
                     delay > policy_.maximum_delay_ms / 2 ? policy_.maximum_delay_ms : delay * 2);
    }
    return delay;
}

std::filesystem::path
SaveCheckpointRetryQueue::normalized_root(const std::filesystem::path& database_root) {
    if (database_root.empty()) {
        return {};
    }
    auto root = database_root.lexically_normal();
    if (root != root.root_path() && root.filename().empty()) {
        root = root.parent_path();
    }
    return root;
}

const char*
save_checkpoint_retry_disposition_name(SaveCheckpointRetryDisposition disposition) noexcept {
    switch (disposition) {
    case SaveCheckpointRetryDisposition::completed:
        return "completed";
    case SaveCheckpointRetryDisposition::scheduled:
        return "scheduled";
    case SaveCheckpointRetryDisposition::exhausted:
        return "exhausted";
    case SaveCheckpointRetryDisposition::terminal_failure:
        return "terminal_failure";
    }
    return "unknown";
}

} // namespace heartstead::save
